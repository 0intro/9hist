#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


void
mmuinit(void)
{
iprint("mmuinit\n");
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


