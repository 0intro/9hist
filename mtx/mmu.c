#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

void
mmuinit(void)
{
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
	int tp;

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
	memset(p->pidonmach, 0, sizeof p->pidonmach);
}

void
putmmu(ulong va, ulong pa, Page *pg)
{
}
