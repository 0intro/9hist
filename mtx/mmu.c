#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

void *ptab;
ulong ptabsize;

void
mmuinit(void)
{
	int lhash, mem;
	extern ulong memsize;	/* passed in from ROM monitor */

	/* heuristically size the hash table */
	lhash = 10;			/* log of hash table size */
	mem = (1<<23);
	while(mem < memsize) {
		lhash++;
		mem <<= 1;
	}

	ptabsize = (1<<(lhash+6));
	ptab = xspanalloc(ptabsize, 0, ptabsize);
	putsdr1(PADDR(ptab) | ((1<<(lhash-10))-1));
}

void
flushmmu(void)
{
	int x;

	x = splhi();
	up->newtlb = 1;
	mmuswitch(up);
	splx(x);
}

/*
 * called with splhi
 */
void
mmuswitch(Proc *p)
{
	int mp;

	if(p->newtlb) {
		p->mmupid = 0;
		p->newtlb = 0;
	}
	mp = p->mmupid;
	if(mp == 0)
		mp = newmmupid();

//	for(i = 0; i < 8; i++)
//		putsr(i, 
}

void
mmurelease(Proc* p)
{
	p->mmupid = 0;
}

void
putmmu(ulong va, ulong pa, Page *pg)
{
}

int
newmmupid(void)
{
	return -1;
}
