#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

int
fault(ulong addr, int read)
{
	ulong mmuvirt, mmuphys, n;
	Seg *s;
	PTE *opte, *pte, *npte;
	Orig *o;
	char *l;
	Page *pg;
	int zeroed = 0, head = 1;
	int i;
	KMap *k, *k1;

	s = seg(u->p, addr);
	if(s == 0){
		if(addr>USTKTOP)
			return -1;
		s = &u->p->seg[SSEG];
		if(s->o==0 || addr<s->maxva-USTACKSIZE || addr>=s->maxva)
			return -1;
		/* grow stack */
		o = s->o;
		n = o->npte;
		if(waserror()){
			pprint("can't allocate stack page\n");
			return -1;
		}
		growpte(o, (s->maxva-addr)>>PGSHIFT);
		poperror();
		/* stacks grown down, sigh */
		lock(o);
		memcpy(o->pte+(o->npte-n), o->pte, n*sizeof(PTE));
		memset(o->pte, 0, (o->npte-n)*sizeof(PTE));
		unlock(o);
		s->minva = addr;
		o->va = addr;
	}else
		o = s->o;
	if(!read && (o->flag&OWRPERM)==0)
		return -1;
	lock(o);
	opte = &o->pte[(addr-o->va)>>PGSHIFT];
	pte = opte;
	if(s->mod){
		while(pte = pte->nextmod)	/* assign = */
			if(pte->proc == u->p){
				if(pte->page==0 || pte->page->va!=addr)
					panic("bad page %lux", pte->page);
				head = 0;
				break;
			}
		if(pte == 0)
			pte = opte;
	}
	if(pte->page == 0){
		if(o->chan==0 || addr>(o->va+(o->maxca-o->minca))){
			/*
			 * Zero fill page.  If we are really doing a copy-on-write
			 * (e.g. into shared bss) we'll move the page later.
			 */
			pte->page = newpage(0, o, addr);
			o->npage++;
			zeroed = 1;
		}else{
			/*
			 * Demand load.  Release o because it could take a while.
			 */
			unlock(o);
			n = (o->va+(o->maxca-o->minca)) - addr;
			if(n > BY2PG)
				n = BY2PG;
			pg = newpage(1, o, addr);
			k = kmap(pg);
			qlock(o->chan);
			if(waserror()){
				kunmap(k);
				qunlock(o->chan);
				pg->o = 0;
				pg->ref--;
				pexit("load i/o error", 0);
			}
			o->chan->offset = (addr-o->va) + o->minca;
			l = (char*)VA(k);
			if((*devtab[o->chan->type].read)(o->chan, l, n) != n)
				error(Eioload);
			qunlock(o->chan);
			if(n<BY2PG)
				memset(l+n, 0, BY2PG-n);
			lock(o);
			kunmap(k);
			poperror();
			opte = &o->pte[(addr-s->minva)>>PGSHIFT];	/* could move */
			pte = opte;
			if(pte->page == 0){
				pte->page = pg;
				o->npage++;
			}else{		/* someone beat us to it */
				pg->o = 0;
				pg->ref--;
			}
		}
	}
	/*
	 * Copy on reference (conf.copymode==1) or write (conf.copymode==0)
	 */
	if((o->flag & OWRPERM) && (conf.copymode || !read)
	&& ((head && ((o->flag&OPURE) || o->nproc>1))
	    || (!head && pte->page->ref>1))){

		/*
		 * Look for the easy way out: are we the last non-modified?
		 */
		if(head && !(o->flag&OPURE)){
			npte = opte;
			for(i=0; npte; i++)
				npte = npte->nextmod;
			if(i == o->nproc)
				goto easy;
		}
		if(head){
			/*
			 * Add to mod list
			 */
			pte = newmod(o);
			pte->proc = u->p;
			pte->page = opte->page;
			pte->page->ref++;
			o->npage++;
			/*
			 * Link into opte mod list (same va)
			 */
			pte->nextmod = opte->nextmod;
			opte->nextmod = pte;
			/*
			 * Link into proc mod list (increasing va)
			 */
			npte = s->mod;
			if(npte == 0){
				s->mod = pte;
				pte->nextva = 0;
			}else{
				while(npte->nextva && npte->nextva->page->va<addr)
					npte = npte->nextva;
				pte->nextva = npte->nextva;
				npte->nextva = pte;
			}
			head = 0;
		}
		pg = pte->page;
		if(zeroed){	/* move page */
			pg->ref--;
			o->npage--;
			opte->page = 0;
		}else{		/* copy page */
			pte->page = newpage(1, o, addr);
			k = kmap(pte->page);
			k1 = kmap(pg);
			memcpy((void*)VA(k), (void*)VA(k1), BY2PG);
			kunmap(k);
			kunmap(k1);
			if(pg->ref <= 1)
				panic("pg->ref <= 1");
			pg->ref--;
		}
    easy:
		mmuphys = PTEWRITE;
	}else{
		mmuphys = PTERONLY;
		if(o->flag & OWRPERM)
			if(o->flag & OPURE){
				if(!head && pte->page->ref==1)
					mmuphys = PTEWRITE;
			}else
				if((head && o->nproc==1)
	  			  || (!head && pte->page->ref==1))
					mmuphys = PTEWRITE;
	}
	mmuvirt = addr;
	mmuphys |= PPN(pte->page->pa) | PTEVALID;
	usepage(pte->page, 1);
	if(pte->page->va != addr)
		panic("wrong addr in tail %lux %lux", pte->page->va, addr);
	if(pte->proc && pte->proc != u->p){
		print("wrong proc in tail %d %s\n", head, u->p->text);
		print("u->p %lux pte->proc %lux\n", u->p, pte->proc);
		panic("addr %lux seg %d wrong proc in tail", addr, s-u->p->seg);
	}
	unlock(o);
	putmmu(mmuvirt, mmuphys);
	return 0;
}

/*
 * Called only in a system call
 */
void
validaddr(ulong addr, ulong len, int write)
{
	Seg *s, *ns;

	if((long)len < 0){
    Err:
		pprint("invalid address in sys call pc %lux sp %lux\n", ((Ureg*)UREGADDR)->pc, ((Ureg*)UREGADDR)->sp);
		postnote(u->p, 1, "sys: bad address", NDebug);
		error(Ebadarg);
	}
    Again:
	s = seg(u->p, addr);
	if(s==0){
		s = &u->p->seg[SSEG];
		if(s->o==0 || addr<s->maxva-USTACKSIZE || addr>=s->maxva)
			goto Err;
	}
	if(write && (s->o->flag&OWRPERM)==0)
		goto Err;
	if(addr+len > s->maxva){
		len -= s->maxva - addr;
		addr = s->maxva;
		goto Again;
	}
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

Seg*
seg(Proc *p, ulong addr)
{
	int i;
	Seg *s;

	for(i=0,s=p->seg; i<NSEG; i++,s++)
		if(s->o && s->minva<=addr && addr<s->maxva)
			return s;
	return 0;
}
