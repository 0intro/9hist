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
	ulong	base;		/* start of page table in kernel virtual space */
	ulong	size;			/* number of bytes in page table */
	ulong	mask;		/* hash mask */
	int		slotgen;		/* next pte (byte offset) when pteg is full */
	int		pidgen;		/* next mmu pid to use */
} ptab;

/*
 *	VSID is 24 bits.  3 are required to distinguish segments in user
 *	space (kernel space only uses the BATs).
 */

#define	VSID(pid, i)	(((pid)<<3)|i)

enum {
	PIDBASE = 1,
	PIDMAX = ((1<<21)-1),
};

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
	ptab.base = (ulong)xspanalloc(ptab.size, 0, ptab.size);
	putsdr1(PADDR(ptab.base) | ((1<<(lhash-10))-1));
	ptab.pidgen = PIDBASE;
	ptab.mask = (1<<lhash)-1;
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
	int i, mp;

	if(p->newtlb) {
		p->mmupid = 0;
		p->newtlb = 0;
	}
	mp = p->mmupid;
	if(mp == 0)
		mp = newmmupid();

	for(i = 0; i < 8; i++)
		putsr(i<<28, VSID(mp, i)|BIT(1)|BIT(2));
}

void
mmurelease(Proc* p)
{
	p->mmupid = 0;
}

void
putmmu(ulong va, ulong pa, Page*)
{
	int mp;
	ulong *p, *ep, *q, pteg;
	ulong vsid, ptehi, x, hash;

	mp = up->mmupid;
	if(mp == 0)
		panic("putmmu pid");

	vsid = VSID(mp, va>>28);
	hash = (vsid ^ (va>>12)&0xffff) & ptab.mask;
	ptehi = PTE0(1, vsid, 0, va);

	pteg = ptab.base + BY2PTEG*hash;
	p = (ulong*)pteg;
	ep = (ulong*)(pteg+BY2PTEG);
	q = nil;
	lock(&ptab);
	tlbflush(va);
	while(p < ep) {
		x = p[0];
		if(x == ptehi) {
			q = p;
if(q[1] == pa) panic("putmmu already set pte");
			break;
		}
		if(q == nil && (x & BIT(0)) == 0)
			q = p;
		p += 2;
	}
	if(q == nil) {
		q = (ulong*)(pteg+ptab.slotgen);
		ptab.slotgen = (ptab.slotgen + BY2PTE) & (BY2PTEG-1);
	}
	q[0] = ptehi;
	q[1] = pa;
	sync();
	unlock(&ptab);
}

int
newmmupid(void)
{
	int pid;

	lock(&ptab);
	pid = ptab.pidgen++;
	unlock(&ptab);
	if(pid > PIDMAX)
		panic("newmmupid");
	up->mmupid = pid;
	return pid;
}
