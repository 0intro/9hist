#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

void
mmuinit(void)
{
}

void
putmmu(ulong va, ulong pa, Page*)
{
	USED(va, pa);
}

void
mmurelease(Proc* proc)
{
	USED(proc);
}

void
mmuswitch(Proc* proc)
{
	USED(proc);
}
