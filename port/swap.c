#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/* Predeclaration */
void	pageout(Proc *p, Segment*);
void	pagepte(int, Segment*, Page**);
int	needpages(void*);
void	pager(void*);
void	executeio(void);
int	canflush(Proc *p, Segment*);

enum
{
	Maxpages = 500,		/* Max number of pageouts per segment pass */
};

Image 	swapimage;
static 	int swopen;
Page	*iolist[Maxpages];
int	ioptr;

void
swapinit(void)
{
	swapalloc.swmap = ialloc(conf.nswap, 0);
	swapalloc.top = &swapalloc.swmap[conf.nswap];
	swapalloc.alloc = swapalloc.swmap;

	swapalloc.free = conf.nswap;
}

ulong
newswap(void)
{
	char *look;
	int n;

	lock(&swapalloc);
	if(swapalloc.free == 0)
		panic("out of swap space");

	n = swapalloc.top - swapalloc.alloc;
	look = swapalloc.alloc;
	while(n && *look) {
		n--;
		look++;
	}
	if(n == 0) {
		look = swapalloc.swmap;
		while(*look)
			look++;
	}
	if(look == swapalloc.top)
		swapalloc.alloc = swapalloc.swmap;
	else
		swapalloc.alloc = look+1;

	*look = 1;
	swapalloc.free--;
	unlock(&swapalloc);
	return (look-swapalloc.swmap) * BY2PG; 
}

void
putswap(Page *p)
{
	lock(&swapalloc);
	if(--swapalloc.swmap[((ulong)p)/BY2PG] == 0)
		swapalloc.free++;
	unlock(&swapalloc);
}

void
dupswap(Page *p)
{
	lock(&swapalloc);
	swapalloc.swmap[((ulong)p)/BY2PG]++;
	unlock(&swapalloc);
}

void
kickpager(void)
{
	static int started;

	if(started)
		wakeup(&swapalloc.r);
	else {
		kproc("pager", pager, 0);
		started = 1;
	}
}

void
pager(void *junk)
{
	Proc *p, *ep;
	Segment *s;
	int i, type;

	if(waserror()) 
		panic("pager: os error\n");

	USED(junk);
	p = proctab(0);
	ep = &p[conf.nproc];

loop:
	u->p->psstate = "Idle";
	sleep(&swapalloc.r, needpages, 0);
	u->p->psstate = "Pageout";

	for(;;) {
		p++;
		if(p > ep)
			p = proctab(0);

		if(p->state == Dead || p->kp)
			continue;

		if(swapimage.c) {
			for(i = 0; i < NSEG; i++) {
				if(!needpages(junk))
					goto loop;
				if(s = p->seg[i]) {
					type = s->type&SG_TYPE;
					switch(type) {
					default:
						break;
					case SG_TEXT:
					case SG_DATA:
					case SG_BSS:
					case SG_STACK:
					case SG_SHARED:
						pageout(p, s);
						executeio();
					}
				}
			}
		}
		else 
		if(palloc.freecount < swapalloc.highwater) {
			/* Rob made me do it ! */
			if(conf.cntrlp == 0)
				freebroken();

			/* Emulate the old system if no swap channel */
			print("no physical memory\n");
			tsleep(&swapalloc.r, return0, 0, 1000);
			wakeup(&palloc.r);
		}
	}
	goto loop;
}

void			
pageout(Proc *p, Segment *s)
{
	Pte **sm, **endsm, *l;
	Page **pg, *entry;
	int type;

	if(!canqlock(&s->lk))	/* We cannot afford to wait, we will surely deadlock */
		return;

	if(s->steal) {		/* Protected by /dev/proc */
		qunlock(&s->lk);
		putseg(s);
		return;
	}

	if(!canflush(p, s)) {	/* Able to invalidate all tlbs with references */
		qunlock(&s->lk);
		putseg(s);
		return;
	}

	if(waserror()) {
		qunlock(&s->lk);
		putseg(s);
		return;
	}

	/* Pass through the pte tables looking for memory pages to swap out */
	type = s->type&SG_TYPE;
	endsm = &s->map[SEGMAPSIZE];
	for(sm = s->map; sm < endsm; sm++) {
		l = *sm;
		if(l == 0)
			continue;
		for(pg = l->first; pg < l->last; pg++) {
			entry = *pg;
			if(pagedout(entry))
				continue;

			if(entry->modref & PG_REF)
				entry->modref &= ~PG_REF;
			else 
				pagepte(type, s, pg);

			if(ioptr >= Maxpages)
				goto out;
		}
	}
out:
	poperror();
	qunlock(&s->lk);
	putseg(s);
	wakeup(&palloc.r);
}

int
canflush(Proc *p, Segment *s)
{
	Proc *ep;
	int i;

	lock(s);
	if(s->ref == 1) {		/* Easy if we are the only user */
		s->ref++;
		unlock(s);
		return canpage(p);
	}
	s->ref++;
	unlock(s);

	/* Now we must do hardwork to ensure all processes which have tlb
	 * entries for this segment will be flushed if we suceed in pageing it out
	 */
	p = proctab(0);
	ep = &p[conf.nproc];
	while(p < ep) {
		if(p->state != Dead) {
			for(i = 0; i < NSEG; i++)
				if(p->seg[i] == s)
					if(!canpage(p))
						return 0;
		}
		p++;
	}

	return 1;						
}

void
pagepte(int type, Segment *s, Page **pg)
{
	ulong daddr;
	Page *outp;

	outp = *pg;
	switch(type) {
	case SG_TEXT:					/* Revert to demand load */
		putpage(outp);
		*pg = 0;
		break;

	case SG_DATA:
	case SG_BSS:
	case SG_STACK:
	case SG_SHARED:
		lockpage(outp);
		outp->ref++;
		uncachepage(outp);
		unlockpage(outp);

		daddr = newswap();
		outp->daddr = daddr;

		/* Enter swap page into cache before segment is unlocked so that
		 * a fault will cause a cache recovery rather than a pagein on a
		 * partially written block.
		 */
		cachepage(outp, &swapimage);
		*pg = (Page*)(daddr|PG_ONSWAP);

		/* Add me to IO transaction list */
		iolist[ioptr++] = outp;
	}
}

void
executeio(void)
{
	Page *out;
	int i, n;
	Chan *c;
	char *kaddr;
	KMap *k;

	c = swapimage.c;

	for(i = 0; i < ioptr; i++) {
		out = iolist[i];

		k = kmap(out);
		kaddr = (char*)VA(k);
		qlock(&c->wrl);

		if(waserror())
			panic("executeio: page out I/O error");

		n = (*devtab[c->type].write)(c, kaddr, BY2PG, out->daddr);
		if(n != BY2PG)
			nexterror();

		qunlock(&c->wrl);
		kunmap(k);
		poperror();

		/* Free up the page after I/O */
		lockpage(out);
		out->ref--;
		unlockpage(out);
		putpage(out);
	}
	ioptr = 0;
}

int
needpages(void *p)
{
	USED(p);
	return palloc.freecount < swapalloc.headroom;
}

void
setswapchan(Chan *c)
{
	if(swapimage.c) {
		if(swapalloc.free != conf.nswap)
			error(Einuse);
		close(swapimage.c);
	}
	incref(c);
	swapimage.c = c;
}

