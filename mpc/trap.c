#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg* ureg, Proc* p)
{
	ureg->pc = p->sched.pc;
	ureg->sp = p->sched.sp+4;
}

/* This routine must save the values of registers the user is not permitted to write
 * from devproc and then restore the saved values before returning
 */
void
setregisters(Ureg* ureg, char* pureg, char* uva, int n)
{
	USED(ureg, pureg, uva, n);
	panic("not done");
}

void
dumpregs(Ureg* ureg)
{
	USED(ureg);
	panic("not done");
}

void
dumpstack(void)
{
	panic("not done");
}

ulong
userpc(void)
{
	Ureg *ureg;

	ureg = (Ureg*)up->dbgreg;
	return ureg->pc;
}

ulong
dbgpc(Proc *p)
{
	Ureg *ureg;

	ureg = p->dbgreg;
	if(ureg == 0)
		return 0;

	return ureg->pc;
}

static void
linkproc(void)
{
	spllo();
	up->kpfun(up->kparg);
}

void
kprocchild(Proc* p, void (*func)(void*), void* arg)
{
	p->sched.pc = (ulong)linkproc;
	p->sched.sp = (ulong)p->kstack+KSTACK;

	p->kpfun = func;
	p->kparg = arg;
}

void
forkchild(Proc *p, Ureg *ureg)
{
	USED(p, ureg);
}

/*
 * called in sysfile.c
 */
void
evenaddr(ulong addr)
{
	if(addr & 3){
		postnote(up, 1, "sys: odd address", NDebug);
		error(Ebadarg);
	}
}

long
execregs(ulong entry, ulong ssize, ulong nargs)
{
	ulong *sp;
	Ureg *ureg;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;

	ureg = up->dbgreg;
	ureg->usp = (ulong)sp;
	ureg->pc = entry;
	return USTKTOP-BY2WD;		/* address of user-level clock */
}

void
trap(Ureg *ur)
{
	USED(ur);
}

/*
 * called directly by l.s:/intrvec
 */
void
intr(Ureg *ur)
{
	USED(ur);
}

void
intrenable(int v, void (*r)(Ureg*, void*), void *arg, int)
{
	USED(v, r, arg);
}

void
reset(void)
{
	IMM *io;

	io = m->iomem;
	io->plprcrk = KEEP_ALIVE_KEY;	// unlock
	io->plprcr |= IBIT(24);		// enable checkstop reset
	putmsr(getmsr() & ~MSR_ME);
	// cause checkstop -> causes reset
	*(ulong*)(0) = 0;
}
