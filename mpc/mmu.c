#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


void
mmuinit(void)
{
	print("mmuinit\n");
	kernelmmu();
}

void
mmuswitch(Proc*)
{
	flushmmu();
}

void
mmurelease(Proc* proc)
{
	USED(proc);
}


void
putmmu(ulong va, ulong pa, Page*)
{
	int x, r;
print("putmmu va=%ux pa=%ux\n", va, pa);
	x = splhi();
	r = _putmmu(va, pa);
	splx(x);
}
