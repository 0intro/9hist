#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

struct
{
	Lock;
	ulong	addr;
	int	active;
	Page	*page;		/* base of Page structures, indexed by phys page number */
	ulong	minppn;		/* index of first usable page */
	Page	*head;		/* most recently used */
	Page	*tail;		/* least recently used */
}palloc;

struct
{
	Lock;
	Orig	*arena;
	Orig	*free;
}origalloc;

typedef union PTEA	PTEA;
union PTEA{
	PTE;
	struct{
		ulong	n;	/* for growpte */
		Orig	*o;	/* for growpte */
		PTEA	*next;	/* for newmod */
	};
};

struct
{
	Lock;
	PTEA	*arena;
	PTEA	*free;
	PTEA	*end;
}ptealloc;

struct{
	Lock;
	PTEA	*free;
}modalloc;

/*
 * Called to allocate permanent data structures, before calling pageinit().
 */
void*
ialloc(ulong n, int align)
{
	ulong p;

	if(palloc.active)
		print("ialloc bad\n");
	if(palloc.addr == 0)
		palloc.addr = ((ulong)&end)&~KZERO;
	if(align){
		palloc.addr += BY2PG-1;
		palloc.addr &= ~(BY2PG-1);
	}
	memset((void*)(palloc.addr|KZERO), 0, n);
	p = palloc.addr;
	palloc.addr += n;
	if(align){
		palloc.addr += BY2PG-1;
		palloc.addr &= ~(BY2PG-1);
	}
	return (void*)(p|KZERO);
}

void
audit(char *s)
{
	int nf, nb;
	Page *p;
	static int here;

	do;while(here);
	here=1;
	p = palloc.head;
	nf=nb=0;
	while(p){
		print("%lux %lux %d\n", p->pa, p->va, p->ref);
		if(p->o){
			print("\t%d %lux %c\n", p->o->nproc, p->o->qid, devchar[p->o->type]);
			delay(100);	/* let it drain; there's a lot here */
		}
		nf++;
		p = p->next;
	}
	p = palloc.tail;
	while(p){
		nb++;
		p = p->prev;
	}
	print("%s: nf: %d nb: %d\n", s, nf, nb);
	delay(1000);
	here=0;
}

void
pageinit(void)
{
	ulong pa, nb, ppn;
	ulong i;
	Page *p;
	PTEA *pte;
	Orig *o;

	ptealloc.arena = ialloc(conf.npte*sizeof(PTEA), 0);
	ptealloc.free = ptealloc.arena;
	ptealloc.end = ptealloc.arena+conf.npte;

	modalloc.free = ialloc(conf.nmod*sizeof(PTEA), 0);

	pte = modalloc.free;
	for(i=0; i<conf.nmod-1; i++,pte++)
		pte->next = pte+1;
	pte->next = 0;

	origalloc.free = ialloc(conf.norig*sizeof(Orig), 0);
	origalloc.arena = origalloc.free;

	o = origalloc.free;
	for(i=0; i<conf.norig-1; i++,o++)
		o->next = o+1;
	o->next = 0;
	palloc.active = 1;

	nb = (conf.npage<<PGSHIFT) - palloc.addr;
	nb -= ((nb+(BY2PG-1))>>PGSHIFT)*sizeof(Page);	/* safe overestimate */
	nb &= ~(BY2PG-1);
	pa = (conf.npage<<PGSHIFT) - nb;	/* physical addr of first free page */
	ppn = pa >> PGSHIFT;			/* physical page number of first free page */
	palloc.page = (Page *)((palloc.addr - ppn*sizeof(Page))|KZERO);
	/*
	 * Now palloc.page[ppn] describes first free page
	 */
	palloc.minppn = ppn;
	palloc.addr = ppn<<PGSHIFT;

	palloc.head = &palloc.page[ppn];
	palloc.tail = &palloc.page[conf.npage-1];
	memset(palloc.head, 0, (conf.npage-ppn)*sizeof(Page));
	print("%lud free pages\n", conf.npage-ppn);
	for(p=palloc.head; p<=palloc.tail; p++,ppn++){
		p->next = p+1;
		p->prev = p-1;
		p->pa = ppn<<PGSHIFT;
	}
	palloc.head->prev = 0;
	palloc.tail->next = 0;
}

Page*
newpage(int noclear, Orig *o, ulong va)
{
	Page *p;
	Orig *o1;
	KMap *k;

	if(palloc.active == 0)
		print("newpage inactive\n");
loop:
	lock(&palloc);
	for(p=palloc.tail; p; p=p->prev){
		if(p->ref == 0)
			goto out;
#ifdef asdf
		if(p->ref == 1){	/* a pageout daemon should do this */
			o1 = p->o;
			if(o1 && o1->nproc==0 && canlock(o1)){
				if(o1->nproc){
					unlock(o1);
					continue;
				}
				print("free %d pages va %lux %lux %c\n", o1->npage, o->va, o1->qid, devchar[o1->type]);
				freepage(o1);
				/* neworig will free the orig and pte's later */
				unlock(o1);
				if(p->ref == 0)
					goto out;
				print("newpage ref != 0");
			}
		}
#endif
	}
	print("no physical memory\n");
	pprint("no physical memory\n");
	unlock(&palloc);
	if(u == 0)
		panic("newpage");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
    out:
	if(p->o){
		print("page in use %lux %lux %lux\n", p->va, p->o, origalloc.arena);
		print("%c %lux %lux, %d %d %d\n", devchar[p->o->type], p->o->va, p->o->qid, p->o->flag, p->o->nproc, p->o->npte);
		panic("shit");
	}
	p->ref = 1;
	usepage(p, 0);
	unlock(&palloc);
	if(!noclear){
		k = kmap(p);
		memset((void*)VA(k), 0, BY2PG);
		kunmap(k);
	}
	p->o = o;
	p->va = va;

	return p;
}

/*
 * Move page to head of list
 */
void
usepage(Page *p, int dolock)
{
	if(dolock)
		lock(&palloc);
	/*
	 * Unlink
	 */
	if(p->prev)
		p->prev->next = p->next;
	else
		palloc.head = p->next;
	if(p->next)
		p->next->prev = p->prev;
	else
		palloc.tail = p->prev;
	/*
	 * Link
	 */
	p->next = palloc.head;
	p->prev = 0;
	if(p->next)
		p->next->prev = p;
	else
		palloc.tail = p;
	palloc.head = p;
	if(dolock)
		unlock(&palloc);
}

/*
 * Move page to tail of list
 */
void
unusepage(Page *p, int dolock)
{
	if(dolock)
		lock(&palloc);
	/*
	 * Unlink
	 */
	if(p->prev)
		p->prev->next = p->next;
	else
		palloc.head = p->next;
	if(p->next)
		p->next->prev = p->prev;
	else
		palloc.tail = p->prev;
	/*
	 * Link
	 */
	p->prev = palloc.tail;
	p->next = 0;
	if(p->prev)
		p->prev->next = p;
	else
		palloc.head = p;
	palloc.tail = p;
	if(dolock)
		unlock(&palloc);
}

Orig*
lookorig(ulong va, ulong npte, int flag, Chan *c)
{
	Orig *o;
	ulong i;

	for(o=origalloc.arena,i=0; i<conf.norig; i++,o++)
		if(o->npage && o->qid==c->qid && o->va==va){
			lock(o);
			if(o->npage && o->qid==c->qid)
			if(o->va==va && o->npte==npte && o->flag==flag)
			if(o->mchan==c->mchan && o->mqid==c->mqid && o->type==c->type){
				if(o->chan == 0){
					o->chan = c;
					incref(c);
				}
				o->nproc++;
				unlock(o);
				return o;
			}
			unlock(o);
		}
	return 0;
}

Orig*
neworig(ulong va, ulong npte, int flag, Chan *c)
{
	Orig *o;
	int i, freed;

	lock(&origalloc);
loop:
	if(o = origalloc.free){		/* assign = */
		origalloc.free = o->next;
		o->va = va;
		o->pte = 0;
		o->flag = flag;
		o->nproc = 1;
		o->npage = 0;
		o->chan = c;
o->nmod = 0;
		if(c){
			o->type = c->type;
			o->qid = c->qid;
			o->mchan = c->mchan;
			o->mqid = c->mqid;
			incref(c);
		}else{
			o->type = -1;
			o->qid = -1;
			o->mqid = -1;
			o->mchan = 0;
		}
		if(u && u->p && waserror()){
			unlock(&origalloc);
			nexterror();
		}
		growpte(o, npte);
		if(u && u->p)
			poperror();
		unlock(&origalloc);
		return o;
	}
	/*
	 * This is feeble.  Orig's should perhaps be held in
	 * an LRU list.  This algorithm is too aggressive.
	 */
	freed = 0;
	for(o=origalloc.arena,i=0; i<conf.norig; i++,o++){
		if(o->nproc==0 && canlock(o)){
			if(o->nproc){
				unlock(o);
				continue;
			}
			freepage(o);
			freepte(o);
			unlock(o);
			o->next = origalloc.free;
			origalloc.free = o;
			freed++;
		}
	}
	if(freed)
		goto loop;
	print("no origs freed\n");
	unlock(&origalloc);
	if(u == 0)
		panic("neworig");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	lock(&origalloc);
	goto loop;
}

PTE*
newmod(Orig *o)
{
	PTEA *pte;

loop:
	lock(&modalloc);
	if(pte = modalloc.free){		/* assign = */
		modalloc.free = pte->next;
		unlock(&modalloc);
		memset(pte, 0, sizeof(PTE));
		return pte;
	}
	unlock(&modalloc);
	print("no mods\n");
DEBUG();
panic("mods %lux %d %d", o->va, o->npte, o->nmod);
	if(u == 0)
		panic("newmod");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

/*
 * Duplicate mod structure for this segment (old) in new process p.
 * old->o must be locked.
 */
void
forkmod(Seg *old, Seg *new, Proc *p)
{
	Orig *o;
	PTE *pte, *ppte, *opte, *npte;
	ulong va;

	o = old->o;
	ppte = 0;
	pte = old->mod;
	while(pte){
if(pte->page==0) panic("forkmod zero page");
if(pte->proc != u->p) panic("forkmod wrong page");
		npte = newmod(o);
o->nmod++;
		npte->proc = p;
		npte->page = pte->page;
		pte->page->ref++;
		o->npage++;
		/*
		 * Link into mod list for this va
		 */
		npte->nextmod = pte->nextmod;
		pte->nextmod = npte;
		/*
		 * Link into mod list for new segment
		 */
		if(ppte == 0)
			new->mod = npte;
		else
			ppte->nextva = npte;
		npte->nextva = 0;
		ppte = npte;
		pte = pte->nextva;
	}
}

void
freesegs(int save)
{
	int i, j;
	Seg *s;
	Orig *o;
	PTE *pte, *opte;
	PTEA *old;
	Page *pg;
	Chan *c;

	s = u->p->seg;
	for(i=0; i<NSEG; i++,s++){
		if(i == save)
			continue;
		o = s->o;
		if(o == 0)
			continue;
		lock(o);
		if(pte = s->mod){	/* assign = */
			while(pte){
				opte = &o->pte[(pte->page->va-o->va)>>PGSHIFT];
				while(opte->nextmod != pte){
					if(opte->page && opte->page->va != pte->page->va)
						panic("pte %lux %lux\n", opte->page->va, pte->page->va);
					opte = opte->nextmod;
					if(opte == 0)
						panic("freeseg opte==0");
				}
				opte->nextmod = pte->nextmod;
				pg = pte->page;
				if(pg->ref == 1){
					pte->page = 0;
					pg->o = 0;
				}
				pg->ref--;
				o->npage--;
o->nmod--;
				old = (PTEA*)pte;
				pte = pte->nextva;
				lock(&modalloc);
				old->next = modalloc.free;
				modalloc.free = old;
				unlock(&modalloc);
			}
		}
		o->nproc--;
		if(o->nproc == 0){
			if(c = o->chan){	/* assign = */
				o->chan = 0;
				close(c);
			}
			if(!(o->flag&OCACHED) || o->npage==0){
				freepage(o);
				freepte(o);
				unlock(o);
				lock(&origalloc);
				o->next = origalloc.free;
				origalloc.free = o;
				unlock(&origalloc);
			}else
				unlock(o);
		}else{
			/*
			 * BUG: there is a leak here. if the origin pte is being used only
			 * by the exiting process, the associated page will linger.  the fix
			 * is to remember either the length of the mod list at each va or
			 * the number of processes sharing the origin pte, and to promote one
			 * of the mods to the origin pte when no process is left using the
			 *  origin pte.
			 */
			unlock(o);
		}
	}
}

/*
 * Adjust segment to desired size.  This implementation is rudimentary.
 */
int
segaddr(Seg *s, ulong min, ulong max)
{
	Orig *o;

	if(max < min)
		return 0;
	if(min != s->minva)	/* can't grow down yet (stacks: fault.c) */
		return 0;
	max = (max+(BY2PG-1)) & ~(BY2PG-1);
	o = s->o;
	if(max == s->maxva)
		return 1;
	if(max > s->maxva){
		/*
		 * Grow
		 */
		/* BUG: check spill onto other segments */
		if(o->va+BY2PG*o->npte < max)
			growpte(o, (max-o->va)>>PGSHIFT);
		s->maxva = max;
		return 1;
	}
	/*
	 * Shrink
	 */
	print("segaddr shrink");
	for(;;);
}

/*
 * o is locked
 */
void
freepage(Orig *o)
{
	PTE *pte;
	Page *pg;
	int i;

	pte = o->pte;
	for(i=0; i<o->npte; i++,pte++)
		if(pg = pte->page){	/* assign = */
			if(pg->ref == 1){
				unusepage(pg, 1);
				pte->page = 0;
				pg->o = 0;
			}
			pg->ref--;
		}
	o->npage = 0;
}

/*
 * Compacting allocator
 */

void
freepte(Orig *o)	/* o is locked */
{
	PTEA *p;
	p = (PTEA*)(o->pte - 1);
	p->o = 0;
}

/*
 * o is locked.  this will always do a grow; if n<=o->npte someone
 * else got here first and we can just return.
 */
void
growpte(Orig *o, ulong n)
{
	PTEA *p;
	ulong nfree;

	lock(&ptealloc);
	lock(o);
	if(o->pte){
		if(o->npte >= n)
			goto Return;
		p = (PTEA*)(o->pte - 1);
		n++;
		if(p+p->n == ptealloc.free){
			if(!compactpte(o, n - p->n))
				goto Trouble;
			p = (PTEA*)(o->pte - 1);
			ptealloc.free += n - p->n;
		}else{
			if(!compactpte(o, n))
				goto Trouble;
			p = ptealloc.free;
			ptealloc.free += n;
			memcpy(p+1, o->pte, o->npte*sizeof(PTE));
			p->o = o;
			((PTEA*)(o->pte-1))->o = 0;
			o->pte = p+1;
		}
		memset(p+1+o->npte, 0, (n-(1+o->npte))*sizeof(PTE));
		p->n = n;
		o->npte = n-1;
		goto Return;
	}
	n++;
	if(!compactpte(o, n))
		goto Trouble;
	p = ptealloc.free;
	ptealloc.free += n;
	memset(p, 0, n*sizeof(PTE));
	p->n = n;
	p->o = o;
	o->pte = p+1;
	o->npte = n-1;
    Return:
	unlock(&ptealloc);
	unlock(o);
	return;

    Trouble:
	unlock(&ptealloc);
	unlock(o);
	if(u && u->p)
		error(0, Enovmem);
	panic("growpte fails %d %lux %d\n", n, o->va, o->npte);
}

int
compactpte(Orig *o, ulong n)
{
	PTEA *p1, *p2;
	Orig *p2o;

	if(ptealloc.end-ptealloc.free >= n)
		return 1;
	p1 = ptealloc.arena;	/* dest */
	p2 = ptealloc.arena;	/* source */
	while(p2 < ptealloc.free){
		p2o = p2->o;
		if(p2o == 0){
    Free:
			p2 += p2->n;
			continue;
		}
		if(p1 != p2){
			if(p2o != o)
				lock(p2o);
			if(p2->o != p2o){	/* freepte()d very recently */
				if(p2->o)
					panic("compactpte p2->o %lux\n", p2->o);
				unlock(p2o);
				goto Free;
			}
			memcpy(p1, p2, p2->n*sizeof(PTE));
			p2o->pte = p1+1;
			if(p2o != o)
				unlock(p2o);
		}
		p2 += p1->n;
		p1 += p1->n;
	}
	ptealloc.free = p1;
	if(ptealloc.end-ptealloc.free >= n)
		return 1;
	unlock(o);
	unlock(&ptealloc);
	if(u && u->p)
		print("%s: %s: ", u->p->text, u->p->pgrp->user);
	print("compactpte fails addr %lux\n", o->va+n*BY2PG);
	return 0;
}
