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

enum
{
	SEMPERPG	= 64,		/* hardware semaphores per page */
	NSEMPG		= 1024,
	ULOCKPG		= 512,
};

struct
{
	Lock	lock;			/* lock to allocate */
	uchar	bmap[NSEMPG];		/* allocation map */
	int	ulockpg;		/* count of user lock available */
}semalloc;

Page lkpgheader[NSEMPG];
#define lhash(laddr)	((int)laddr>>2)&(((NSEMPG-ULOCKPG)*(BY2PG>>2))-1)

void
lockinit(void)
{
	memset(semalloc.bmap, 0, sizeof(semalloc.bmap));
	/*
	 * Initialise the system semaphore hardware
	 */
	memset(SBSEM, 0, (NSEMPG-ULOCKPG)*BY2PG);
	semalloc.ulockpg = ULOCKPG;
}

/* Moral equivalent of newpage for pages of hardware locks */
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

void
lock(Lock *lk)
{
	int *hwsem;
	int i, hash;

	hash = lhash(lk);
	hwsem = (int*)SBSEM+hash;

	i = 1000000;
	for(;;) {
		if((*hwsem & 1) == 0) {
			if(lk->val)
				*hwsem = 0;
			else {
				lk->val = 1;
				*hwsem = 0;
				lk->pc = getcallerpc(lk);
				return;
			}
		}
		while(lk->val && i)
			i--;
		if(i <= 0)
			break;
	}
	print("lock loop %lux pc %lux held by pc %lux\n", lk, getcallerpc(lk), lk->pc);
	dumpstack();
}	

int
canlock(Lock *lk)
{
	int *hwsem;
	int i, hash;

	hash = lhash(lk);
	hwsem = (int*)SBSEM+hash;

	for(;;) {
		if((*hwsem & 1) == 0) {
			if(lk->val)
				*hwsem = 0;
			else {
				lk->val = 1;
				*hwsem = 0;
				lk->pc = getcallerpc(lk);
				return 1;
			}
		}
		if(lk->val)
			return 0;
	}
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->val = 0;
}
