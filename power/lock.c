#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

/*
 * The hardware semaphores are strange.  64 per page, replicated 16 times
 * per page, 1024 pages of them.  Only the low bit is meaningful.
 * Reading an unset semaphore sets the semaphore and returns the old value.
 * Writing a semaphore sets the value, so writing 0 resets (clears) the semaphore.
 */

#define	SEMPERPG	64		/* hardware semaphores per page */
#define NSEMPG		1024
#define ULOCKPG		512

struct
{
	Lock	lock;			/* lock to allocate */
	ulong	*nextsem;		/* next one to allocate */
	int	nsem;			/* at SEMPERPG, jump to next page */
	uchar	bmap[NSEMPG];		/* allocation map */
	int	ulockpg;		/* count of user lock available */
}semalloc;

Page lkpgheader[NSEMPG];

void
lockinit(void)
{
	memset(semalloc.bmap, 0, sizeof(semalloc.bmap));
	semalloc.bmap[0] = 1;

	semalloc.ulockpg = ULOCKPG;
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
lkpage(ulong va)
{
	uchar *p, *top;
	Page *pg;
	int i;

	lock(&semalloc.lock);
	if(--semalloc.ulockpg < 0) {
		semalloc.ulockpg++;
		unlock(&semalloc.lock);
		return 0;
	}
	top = &semalloc.bmap[NSEMPG];
	for(p = semalloc.bmap; *p && p < top; p++)
		;
	if(p == top)
		panic("lkpage");

	*p = 1;
	i = p-semalloc.bmap;
	pg = &lkpgheader[i];
	pg->pa = (ulong)((i*WD2PG) + SBSEM) & ~UNCACHED;
	pg->va = va;
	pg->ref = 1;

	unlock(&semalloc.lock);
	return pg;
}

void
lkpgfree(Page *pg)
{
	uchar *p;

	lock(&semalloc.lock);
	p = &semalloc.bmap[((pg->pa|UNCACHED)-(ulong)SBSEM)/BY2PG];
	if(!*p)
		panic("lkpgfree");
	*p = 0;
	
	semalloc.ulockpg++;
	unlock(&semalloc.lock);
}

/*
 * If l->sbsem is zero, allocate a hardware semaphore first.
 */
void
lock(Lock *l)
{
	int i;
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(l->sbsem == 0){
			if(semalloc.nsem == SEMPERPG){
				semalloc.nsem = 0;
				semalloc.nextsem = lkpgalloc();
			}
			l->sbsem = semalloc.nextsem;
			semalloc.nextsem++;
			semalloc.nsem++;
			unlock(l);		/* put sem in known state */
		}
		unlock(&semalloc.lock);
		sbsem = l->sbsem;
	}
	/*
	 * Try the fast grab first
	 */
	if((*sbsem&1) == 0){
		l->pc = getcallerpc(l);
		return;
	}
	for(i=0; i<10000000; i++)
    		if((*sbsem&1) == 0){
			l->pc = getcallerpc(l);
			return;
	}
	*sbsem = 0;
	print("lock loop %lux pc %lux held by pc %lux\n", l, getcallerpc(l), l->pc);
	dumpstack();
}

int
canlock(Lock *l)
{
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(l->sbsem == 0){
			if(semalloc.nsem == SEMPERPG){
				semalloc.nsem = 0;
				semalloc.nextsem = lkpgalloc();
			}
			l->sbsem = semalloc.nextsem;
			semalloc.nextsem++;
			semalloc.nsem++;
			unlock(l);		/* put sem in known state */
		}
		unlock(&semalloc.lock);
		sbsem = l->sbsem;
	}
	if(*sbsem & 1)
		return 0;
	l->pc = getcallerpc(l);
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	*l->sbsem = 0;
}
