#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/*
 *	The page table is shared across all processes and processors
 *	(hence needs to be locked for updates on a multiprocessor).
 *	Different processes are distinguished via the VSID field in
 *	the segment registers.  As flushing the entire page table is an
 *	expensive operation, we implement an aging algorithm for
 *	mmu pids, with a background kproc to purge stale pids en mass.
 */

static struct {
	Lock;
	void		*base;		/* start of page table in kernel virtual space */
	ulong	size;			/* number of bytes in page table */
	int		slotgen;		/* used to choose which pte to alocate in pteg */
} ptab;

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

	ptab.size = (1<<(lhash+6));
	ptab.base = xspanalloc(ptab.size, 0, ptab.size);
	putsdr1(PADDR(ptab.base) | ((1<<(lhash-10))-1));
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
