#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

struct Intrregs
{
	ulong	icip;	/* pending IRQs */
	ulong	icmr;	/* IRQ mask */
	ulong	iclr;	/* IRQ if bit == 0, FRIQ if 1 */
	ulong	iccr;	/* control register */
	ulong	icfp;	/* pending FIQs */
	ulong	dummy1[3]
	ulong	icpr;	/* pending interrupts */
	
};

struct Intrregs *intrregs;

typedef struct Vctl {
	Vctl*	next;			/* handlers on this vector */

	char	name[NAMELEN];		/* of driver */
	int	irq;

	void	(*f)(Ureg*, void*);	/* handler to call */
	void*	a;			/* argument to call it with */
} Vctl;

static Lock vctllock;
static Vctl *vctl[32];

/*
 *  set up for exceptioons
 */
void
trapinit(void)
{
	/*
	 *  exceptionvectors points to a prototype in l.s of the
	 *  exception vectors that save the regs and then call
	 *  trap().  The actual vectorrs are double mapped
	 *  to 0xffff0000 and to KZERO.  We write them via
	 *  KZERO since a data access to them will cause an
	 *  exception.
	 */
	memmove((void*)KZERO, exceptionvectors, 2*4*8);
	wbflush();

	/* map in interrupt registers */
	intrregs = mapspecial(INTRREGS, sizeof(*intrregs));

	/* make all interrupts irq and disable all interrupts */
	intrregs->iclr = 0;
	intrregs->icmr = 0;
}

/*
 *  enable an interrupt and attach a function to i
 */
void
intrenable(int irq, void (*f)(Ureg*, void*), void* a, char *name)
{
	int vno;
	Vctl *v;

	v = xalloc(sizeof(Vctl));
	v->irq = irq;
	v->f = f;
	v->a = a;
	strncpy(v->name, name, NAMELEN-1);
	v->name[NAMELEN-1] = 0;

	lock(&vctllock);
	v->next = vctl[irq];
	vctl[irq] = v;
	unlock(&vctllock);
}

/*
 *  here on all exceptions with registers saved
 */
void
trap(Ureg *ureg)
{
	switch(ureg->type){
	case PsrMfiq:	/* fast interrupt */
		panic("fiq can't happen");
		break;
	case PsrMabt:	/* fault */
	case PsrMabt+1:	/* fault */
		panic("faults not implemented");
		break;
	case PsrMund:	/* undefined instruction */
		panic("undefined instruction");
		break;
	case PsrMirq:	/* device interrupt */
		break;
	case PsrMsvc:	/* system call */
		break;
	}
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg *ureg, Proc *p)
{
	USED(ureg, p);
}

/*
 *  return the userpc the last exception happened at
 */
ulong
userpc(void)
{
	return 0;
}

/* This routine must save the values of registers the user is not permitted
 * to write from devproc and then restore the saved values before returning.
 */
void
setregisters(Ureg* ureg, char* pureg, char* uva, int n)
{
	USED(ureg, pureg, uva, n);
}

/*
 *  this is the body for all kproc's
 */
static
void
linkproc(void)
{
	spllo();
	up->kpfun(up->kparg);
}

/*
 *  setup stack and initial PC for a new kernel proc.  This is architecture
 *  dependent because of the starting stack location
 */
void
kprocchild(Proc *p, void (*func)(void*), void *arg)
{
	p->sched.pc = (ulong)linkproc;
	p->sched.sp = (ulong)p->kstack+KSTACK;

	p->kpfun = func;
	p->kparg = arg;
}


/* 
 *  Craft a return frame which will cause the child to pop out of
 *  the scheduler in user mode with the return register zero.  Set
 *  pc to point to a l.s return function.
 */
void
forkchild(Proc *p, Ureg *ureg)
{
	USED(p, ureg);
}

/*
 *  setup stack, initial PC, and any arch dependent regs for an execing user proc.
 */
long
execregs(ulong entry, ulong ssize, ulong nargs)
{
	ulong *sp;
	Ureg *ureg;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;

	ureg = up->dbgreg;
	ureg->r13 = (ulong)sp;
	ureg->pc = entry;
	return USTKTOP-BY2WD;		/* address of user-level clock */
}

/*
 *  dump the processor stack for this process
 */
void
dumpstack(void)
{
}


ulong
dbgpc(Proc *p)
{
	USED(p);
	return 0;
}
