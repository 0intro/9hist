#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "errno.h"

/*
 * The hardware semaphores are strange.  64 per page, replicated 16 times
 * per page, 1024 pages of them.  Only the low bit is meaningful.
 * Reading an unset semaphore sets the semaphore and returns the old value.
 * Writing a semaphore sets the value, so writing 0 resets (clears) the semaphore.
 */

#define	SEMPERPG	64		/* hardware semaphores per page */
#define NSEMPG		1024

struct
{
	Lock	lock;			/* lock to allocate */
	ulong	*nextsem;		/* next one to allocate */
	int	nsem;			/* at SEMPERPG, jump to next page */
	uchar	bmap[NSEMPG];		/* allocation map */
}semalloc;

Page lkpgheader[NSEMPG];

void
lockinit(void)
{
	memset(semalloc.bmap, 0, sizeof(semalloc.bmap));
	semalloc.bmap[0] = 1;

	semalloc.lock.sbsem = SBSEM;
	semalloc.nextsem = SBSEM+1;
	semalloc.nsem = 1;
	unlock(&semalloc.lock);
}

/* return the address of the next free page of locks */
ulong*
lkpgalloc(void)
{
	uchar *p, *top;

	top = &semalloc.bmap[NSEMPG];
	for(p = semalloc.bmap; *p && p < top; p++)
		;
	if(p == top)
		panic("lkpgalloc");

	*p = 1;
	return (p-semalloc.bmap)*WD2PG + SBSEM;
}

/* Moral equivalent of newpage for pages of hardware lock */
Page*
lkpage(Orig *o, ulong va)
{
	uchar *p, *top;
	Page *pg;
	int i;

	lock(&semalloc.lock);
	top = &semalloc.bmap[NSEMPG];
	for(p = semalloc.bmap; *p && p < top; p++)
		;
	if(p == top)
		panic("lkpage");

	*p = 1;
	i = p-semalloc.bmap;
	pg = &lkpgheader[i];
	pg->pa = (ulong)((i*WD2PG) + SBSEM);
	pg->va = va;
	pg->o = o;

	unlock(&semalloc.lock);
	return pg;
}

void
lkpgfree(ulong *lpa)
{
	uchar *p;

	p = &semalloc.bmap[(lpa-SBSEM)/WD2PG];
	if(!*p)
		panic("lkpgfree");
	*p = 0;
}

#define PCOFF -9

/*
 * If l->sbsem is zero, allocate a hardware semaphore first.
 */
void
lock(Lock *ll)
{
	Lock *l = ll;
	int i;
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(semalloc.nsem == SEMPERPG){
			semalloc.nsem = 0;
			semalloc.nextsem = lkpgalloc();
		}
		l->sbsem = semalloc.nextsem;
		semalloc.nextsem++;
		semalloc.nsem++;
		unlock(&semalloc.lock);
		unlock(l);		/* put sem in known state */
		sbsem = l->sbsem;
	}
	/*
	 * Try the fast grab first
	 */
	if((*sbsem&1) == 0){
		l->pc = ((ulong*)&ll)[PCOFF];
		return;
	}
	for(i=0; i<10000000; i++)
    		if((*sbsem&1) == 0){
			l->pc = ((ulong*)&ll)[PCOFF];
			return;
	}
	*sbsem = 0;
	print("lock loop %lux pc %lux held by pc %lux\n", l, ((ulong*)&ll)[PCOFF], l->pc);
	dumpstack();
}

int
canlock(Lock *l)
{
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(semalloc.nsem == SEMPERPG){
			semalloc.nsem = 0;
			semalloc.nextsem = lkpgalloc();
		}
		l->sbsem = semalloc.nextsem;
		semalloc.nextsem++;
		semalloc.nsem++;
		unlock(&semalloc.lock);
		unlock(l);		/* put sem in known state */
		sbsem = l->sbsem;
	}
	if(*sbsem & 1)
		return 0;
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	*l->sbsem = 0;
}

void
mklockseg(Seg *s)
{
	Orig *o;

	s->proc = u->p;
	o = neworig(LKSEGBASE, 0, OWRPERM|OPURE, 0);
	o->minca = 0;
	o->maxca = 0;
	s->o = o;
	s->minva = LKSEGBASE;
	s->maxva = LKSEGBASE;
	s->mod = 0;
}
