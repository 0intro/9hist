#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#define TLBINVLAID	KZERO

void
mmuinit(void)
{
	int i;

	print("mmuinit\n");
}

void
flushmmu(void)
{
	int x;

if(0)print("flushmmu(%ld)\n", up->pid);
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
	int tp;

if(0)print("mmuswitch()\n");
	if(p->newtlb) {
		memset(p->pidonmach, 0, sizeof p->pidonmach);
		p->newtlb = 0;
	}
	tp = p->pidonmach[m->machno];
//	putcasid(tp);
}

void
mmurelease(Proc* p)
{
if(0)print("mmurelease(%ld)\n", p->pid);
	memset(p->pidonmach, 0, sizeof p->pidonmach);
}

void
putmmu(ulong va, ulong pa, Page *pg)
{
}
