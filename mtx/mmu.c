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

static ulong	ptabsize;			/* number of bytes in page table */
static ulong	ptabmask;		/* hash mask */

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
	ulong v;

	if(ptabsize == 0) {
		/* heuristically size the hash table */
		lhash = 10;
		mem = (1<<23);
		while(mem < memsize) {
			lhash++;
			mem <<= 1;
		}
		ptabsize = (1<<(lhash+6));
		ptabmask = (1<<lhash)-1;
	}

	m->ptabbase = (ulong)xspanalloc(ptabsize, 0, ptabsize);
	putsdr1(PADDR(m->ptabbase) | (ptabmask>>10));
	m->mmupid = PIDBASE;

	v = getdec();
	memset((void*)m->ptabbase, 0, ptabsize);
	v -= getdec();
	print("memset took %lud cycles, dechz %lud\n", v, m->dechz);
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

	if(p->kp) {
		for(i = 0; i < 8; i++)
			putsr(i<<28, 0);
		return;
	}

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
putmmu(ulong va, ulong pa, Page *pg)
{
	int mp;
	char *ctl;
	ulong *p, *ep, *q, pteg;
	ulong vsid, ptehi, x, hash;

	mp = up->mmupid;
	if(mp == 0)
		panic("putmmu pid");

	vsid = VSID(mp, va>>28);
	hash = (vsid ^ (va>>12)&0xffff) & ptabmask;
	ptehi = PTE0(1, vsid, 0, va);

	pteg = m->ptabbase + BY2PTEG*hash;
	p = (ulong*)pteg;
	ep = (ulong*)(pteg+BY2PTEG);
	q = nil;
	tlbflush(va);
	while(p < ep) {
		x = p[0];
		if(x == ptehi) {
			q = p;
if(q[1] == pa) print("putmmu already set pte\n");
			break;
		}
		if(q == nil && (x & BIT(0)) == 0)
			q = p;
		p += 2;
	}
	if(q == nil) {
		q = (ulong*)(pteg+m->slotgen);
		m->slotgen = (m->slotgen + BY2PTE) & (BY2PTEG-1);
	}
	q[0] = ptehi;
	q[1] = pa;
	sync();

	ctl = &pg->cachectl[m->machno];
	switch(*ctl) {
	case PG_NEWCOL:
	default:
		panic("putmmu: %d\n", *ctl);
		break;
	case PG_NOFLUSH:
		break;
	case PG_TXTFLUSH:
		dcflush((void*)pg->va, BY2PG);
		icflush((void*)pg->va, BY2PG);
		*ctl = PG_NOFLUSH;
		break;
	}
}

int
newmmupid(void)
{
	int pid;

	pid = m->mmupid++;
	if(m->mmupid > PIDMAX)
		panic("ran out of mmu pids");
//		m->mmupid = PIDBASE;
	up->mmupid = pid;
	return pid;
}
