#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

void
fault(Ureg *ur, int user, int code)
{
	ulong addr, mmuvirt, mmuphys, n;
	extern char *excname[];
	Seg *s;
	PTE *opte, *pte, *npte;
	Orig *o;
	char *l;
	Page *pg;
	int zeroed = 0, head = 1;
	int i;

	addr = ur->badvaddr;
	addr &= ~(BY2PG-1);

	s = seg(u->p, addr);
	if(s == 0){
		if(addr>USTKTOP){
	    cant:
			if(user){
				pprint("user %s badvaddr=0x%lux\n", excname[code], ur->badvaddr);
				pprint("status=0x%lux pc=0x%lux sp=0x%lux\n", ur->status, ur->pc, ur->sp);
				pexit("Suicide", 0);
			}
			print("kernel %s badvaddr=0x%lux\n", excname[code], ur->badvaddr);
			print("status=0x%lux pc=0x%lux sp=0x%lux\n", ur->status, ur->pc, ur->sp);
			u->p->state = MMUing;
			dumpregs(ur);
			panic("fault");
		}
		s = &u->p->seg[SSEG];
		if(s->o==0 || addr<s->maxva-4*1024*1024 || addr>=s->maxva)
			goto cant;
		/* grow stack */
		o = s->o;
		n = o->npte;
		growpte(o, (s->maxva-addr)>>PGSHIFT);
		/* stacks grown down, sigh */
		lock(o);
		memcpy(o->pte+(o->npte-n), o->pte, n*sizeof(PTE));
		memset(o->pte, 0, (o->npte-n)*sizeof(PTE));
		unlock(o);
		s->minva = addr;
		o->va = addr;
	}else
		o = s->o;
	if((code==CTLBM || code==CTLBS) && (o->flag&OWRPERM)==0)
		goto cant;
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
			qlock(o->chan);
			if(waserror()){
				print("demand load i/o error %d\n", u->error.code);
				qunlock(o->chan);
				pg->o = 0;
				pg->ref--;
				goto cant;
			}
			o->chan->offset = (addr-o->va) + o->minca;
			l = (char*)(pg->pa|KZERO);
			if((*devtab[o->chan->type].read)(o->chan, l, n) != n)
				error(0, Eioload);
			qunlock(o->chan);
			poperror();
			/* BUG: if was first page of bss, move to data */
			if(n<BY2PG)
				memset(l+n, 0, BY2PG-n);
			lock(o);
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
	 * Copy on reference
	 */
	if((o->flag & OWRPERM)
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
			pte = newmod();
o->nmod++;
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
			memcpy((void*)(pte->page->pa|KZERO), (void*)(pg->pa|KZERO), BY2PG);
			if(pg->ref <= 1)
				panic("pg->ref <= 1");
			pg->ref--;
		}
    easy:
		mmuphys = PTEWRITE;
	}else{
		mmuphys = 0;
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
	mmuphys |= pte->page->pa | PTEVALID;
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
}

/*
 * Called only in a system call
 */
void
validaddr(ulong addr, ulong len, int write)
{
	Seg *s;

	if((long)len < 0)
		goto Err;
	s = seg(u->p, addr);
	if(s==0 || addr+len>s->maxva || (write && (s->o->flag&OWRPERM)==0)){
    Err:
		pprint("invalid address in sys call pc %lux sp %lux\n", ((Ureg*)UREGADDR)->pc, ((Ureg*)UREGADDR)->sp);
		postnote(u->p, 1, "bad address", NDebug);
		error(0, Ebadarg);
	}
}

void
evenaddr(ulong addr)
{
	if(addr & 3){
		postnote(u->p, 1, "odd address", NDebug);
		error(0, Ebadarg);
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
