#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

#define DPRINT

int
fault(ulong addr, int read)
{
	ulong mmuphys=0, soff;
	Segment *s;
	Pte **p;
	Page **pg, *lkp, *new = 0;
	int type;
	char *sps;

	sps = u->p->psstate;
	u->p->psstate = "Fault";

	m->pfault++;
again:
	s = seg(u->p, addr, 1);
	if(s == 0) {
		u->p->psstate = sps;
		return -1;
	}

	if(!read && (s->type&SG_RONLY)) {
		qunlock(&s->lk);
		u->p->psstate = sps;
		return -1;
	}

	addr &= ~(BY2PG-1);
	soff = addr-s->base;
	p = &s->map[soff/PTEMAPMEM];
	if(*p == 0) 
		*p = ptealloc();

	pg = &(*p)->pages[(soff&(PTEMAPMEM-1))/BY2PG];
	type = s->type&SG_TYPE;

	switch(type) {
	case SG_TEXT:
		if(pagedout(*pg)) 		/* No data - must demand load */
			pio(s, addr, soff, pg);
		
		mmuphys = PPN((*pg)->pa) | PTERONLY|PTEVALID;
		(*pg)->modref = PG_REF;
		break;
	case SG_SHARED:
	case SG_BSS:
	case SG_STACK:	
			/* Zero fill on demand */
		if(*pg == 0) {			
			if(new == 0 && (new = newpage(1, &s, addr)) && s == 0)
				goto again;
			*pg = new;
			new = 0;
		}
						/* NO break */
	case SG_DATA:
		if(pagedout(*pg))
			pio(s, addr, soff, pg);

		if(type == SG_SHARED)
			goto done;

		if(read && conf.copymode == 0) {
			mmuphys = PPN((*pg)->pa) | PTERONLY|PTEVALID;
			(*pg)->modref |= PG_REF;
			break;
		}

		lkp = *pg;
		lockpage(lkp);
		if(lkp->ref > 1) {
			unlockpage(lkp);
			if(new == 0 && (new = newpage(0, &s, addr)) && s == 0)
				goto again;
			*pg = new;
			new = 0;
			copypage(lkp, *pg);
			putpage(lkp);
		}
		else {
			/* put a duplicate of a text page back onto the free list */
			if(lkp->image)     
				duppage(lkp);	
		
			unlockpage(lkp);
		}
	done:
		mmuphys = PPN((*pg)->pa) | PTEWRITE|PTEVALID;
		(*pg)->modref = PG_MOD|PG_REF;
		break;
	case SG_PHYSICAL:
		if(*pg == 0)
			*pg = (*s->pgalloc)(addr);

		mmuphys = PPN((*pg)->pa) | PTEWRITE|PTEUNCACHED|PTEVALID;
		(*pg)->modref = PG_MOD|PG_REF;
		break;
	default:
		panic("fault");
	}

	qunlock(&s->lk);

	/*
	 * A race may provide a page we never used when	
	 * a fault is fixed while process slept in newpage
	 */
	if(new)
		putpage(new);

	putmmu(addr, mmuphys, *pg);
	u->p->psstate = sps;
	return 0;
}

void
pio(Segment *s, ulong addr, ulong soff, Page **p)
{
	Page *new;
	KMap *k;
	Chan *c;
	int n, ask;
	char *kaddr;
	ulong daddr;
	Page *loadrec;

	loadrec = *p;
	if(loadrec == 0) {
		daddr = s->fstart+soff;		/* Compute disc address */
		new = lookpage(s->image, daddr);
	}
	else {
		daddr = swapaddr(loadrec);
		new = lookpage(&swapimage, daddr);
		if(new)
			putswap(loadrec);
	}

	if(new) {				/* Page found from cache */
		*p = new;
		return;
	}

	qunlock(&s->lk);

	new = newpage(0, 0, addr);
	k = kmap(new);
	kaddr = (char*)VA(k);
	
	if(loadrec == 0) {			/* This is demand load */
		c = s->image->c;
		qlock(&c->rdl);
		/* We are unable to correctly error up throught the kernel
		 * during faults. Therefore note delivery is postponed until
		 * we leave the system. Read errors cause the process to die.
		 */
		while(waserror()) {
			if(strcmp(u->error, errstrtab[Eintr]) == 0)
				continue;
			qunlock(&c->rdl);
			kunmap(k);
			putpage(new);
			pexit("demand load I/O error", 0);
		}

		ask = s->flen-soff;
		if(ask > BY2PG)
			ask = BY2PG;
		n = (*devtab[c->type].read)(c, kaddr, ask, daddr);
		if(n != ask)
			error(Eioload);
		if(ask < BY2PG)
			memset(kaddr+ask, 0, BY2PG-ask);

		poperror();
		kunmap(k);
		qunlock(&c->rdl);
		qlock(&s->lk);
		if(*p == 0) { 			/* Someone may have got there first */
			new->daddr = daddr;
			cachepage(new, s->image);
			*p = new;
			if(s->flushme)
				memset(new->cachectl, PG_TXTFLUSH, sizeof(new->cachectl));
		}
		else 
			putpage(new);
	}
	else {					/* This is paged out */
		c = swapimage.c;
		qlock(&c->rdl);

		if(waserror()) {
			qunlock(&c->rdl);
			kunmap(k);
			putpage(new);
			qlock(&s->lk);
			qunlock(&s->lk);
			pexit("page in I/O error", 0);
		}

		n = (*devtab[c->type].read)(c, kaddr, BY2PG, daddr);
		if(n != BY2PG)
			error(Eioload);

		poperror();
		kunmap(k);
		qunlock(&c->rdl);
		qlock(&s->lk);

		if(pagedout(*p)) {
			new->daddr = daddr;
			cachepage(new, &swapimage);
			putswap(*p);
			*p = new;
			if(s->flushme)
				memset(new->cachectl, PG_TXTFLUSH, sizeof(new->cachectl));
		}
		else
			putpage(new);
	}
}

/*
 * Called only in a system call
 */
void
validaddr(ulong addr, ulong len, int write)
{
	Segment *s;

	if((long)len >= 0) {
		for(;;) {
			s = seg(u->p, addr, 0);
			if(s == 0 || (write && (s->type&SG_RONLY)))
				break;

			if(addr+len > s->top) {
				len -= s->top - addr;
				addr = s->top;
				continue;
			}
			return;
		}
	}

	pprint("invalid address %lux in sys call pc %lux sp %lux\n", 
			addr, ((Ureg*)UREGADDR)->pc, ((Ureg*)UREGADDR)->sp);

	postnote(u->p, 1, "sys: bad address", NDebug);
	error(Ebadarg);
}
  
/*
 * &s[0] is known to be a valid address.
 */
void*
vmemchr(void *s, int c, int n)
{
	int m;
	char *t;
	ulong a;

	a = (ulong)s;
	m = BY2PG - (a & (BY2PG-1));
	if(m < n){
		t = vmemchr(s, c, m);
		if(t)
			return t;
		if(!(a & KZERO))
			validaddr(a+m, 1, 0);
		return vmemchr((void*)(a+m), c, n-m);
	}
	/*
	 * All in one page
	 */
	return memchr(s, c, n);
}

Segment*
seg(Proc *p, ulong addr, int dolock)
{
	Segment **s, **et, *n;

	et = &p->seg[NSEG];
	for(s = p->seg; s < et; s++)
		if(n = *s){
			if(addr >= n->base && addr < n->top) {
				if(dolock == 0)
					return n;
	
				qlock(&n->lk);
				if(addr >= n->base && addr < n->top)
					return n;
				qunlock(&n->lk);
			}
		}

	return 0;
}
