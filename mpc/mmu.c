#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


static void
taskswitch(ulong pdb, ulong stack)
{
	USED(pdb, stack);
}

void
mmuinit(void)
{
}

void
flushmmu(void)
{
}

static void
mmuptefree(Proc* proc)
{
	USED(proc);
}

void
mmuswitch(Proc* proc)
{
	USED(proc);
}

void
mmurelease(Proc* proc)
{
	USED(proc);
}

static Page*
mmupdballoc(void)
{
}

void
putmmu(ulong va, ulong pa, Page*)
{
	USED(va, pa);
}

static Lock mmukmaplock;

int
mmukmapsync(ulong va)
{
	USED(va);
	return 0;
}

ulong
mmukmap(ulong pa, ulong va, int size)
{
	USED(pa, va, size);
	return 0;
}
