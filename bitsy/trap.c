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
	ulong	dummy1[3];
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
 *   Layout at virtual address 0.
 */
typedef struct Vpage0 {
	void	(*vectors[8])(void);
	ulong	vtable[8];

	ulong	hole[16];
} Vpage0;
Vpage0 *vpage0;

/*
 *  set up for exceptioons
 */
void
trapinit(void)
{
	/* set up the exception vectors */
	vpage0 = (Vpage0*)EVECTORS;
	memmove(vpage0->vectors, vectors, sizeof(vpage0->vectors));
	memmove(vpage0->vtable, vtable, sizeof(vpage0->vtable));
	memset(vpage0->hole, 0, sizeof(vpage0->hole));
	wbflush();

	/* use exception vectors at 0xFFFF0000 */
	mappedIvecEnable();

	/* set up the stacks for the interrupt modes */
	setr13(PsrMfiq, m->sfiq);
	setr13(PsrMirq, m->sirq);
	setr13(PsrMabt, m->sabt);
	setr13(PsrMund, m->sund);

	/* map in interrupt registers */
	intrregs = mapspecial(INTRREGS, sizeof(*intrregs));

	/* make all interrupts IRQ (i.e. not FIQ) and disable all interrupts */
	intrregs->iclr = 0;
	intrregs->icmr = 0;
}

void
trapdump(char *tag)
{
	iprint("%s: icip %lux icmr %lux iclr %lux iccr %lux icfp %lux\n",
		tag, intrregs->icip, intrregs->icmr, intrregs->iclr,
		intrregs->iccr, intrregs->icfp);
}

void
dumpregs(Ureg *ur)
{
	iprint("r0  0x%.8lux r1  0x%.8lux r3  0x%.8lux r3  0x%.8lux\n",
		ur->r0, ur->r1, ur->r2, ur->r3);
	iprint("r4  0x%.8lux r5  0x%.8lux r6  0x%.8lux r7  0x%.8lux\n",
		ur->r4, ur->r5, ur->r6, ur->r7);
	iprint("r8  0x%.8lux r9  0x%.8lux r10 0x%.8lux r11 0x%.8lux\n",
		ur->r8, ur->r9, ur->r10, ur->r11);
	iprint("r12 0x%.8lux r13 0x%.8lux r14 0x%.8lux\n",
		ur->r12, ur->r13, ur->r14);
	iprint("type %.8lux psr %.8lux pc %.8lux\n", ur->type, ur->psr, ur->pc);
}

/*
 *  enable an interrupt and attach a function to it
 */
void
intrenable(int irq, void (*f)(Ureg*, void*), void* a, char *name)
{
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
	intrregs->icmr |= 1<<irq;
	unlock(&vctllock);
}

/*
 *  called by trap to handle access faults
 */
static void
faultarm(Ureg *ureg, ulong va, int user, int read)
{
	int n, insyscall;
	char buf[ERRLEN];

	insyscall = up->insyscall;
	up->insyscall = 1;
	n = fault(va, read);
iprint("fault returns %d\n", n);
	if(n < 0){
		if(!user){
			dumpregs(ureg);
			panic("fault: 0x%lux\n", va);
		}
		sprint(buf, "sys: trap: fault %s va=0x%lux",
			read? "read" : "write", va);
		postnote(up, 1, buf, NDebug);
	}
	up->insyscall = insyscall;
}

/*
 *  here on all exceptions other than syscall (SWI)
 */
void
trap(Ureg *ureg)
{
	int i;
	Vctl *v;
	int user;
	ulong pc, va;
	char buf[ERRLEN];

	user = (ureg->psr & PsrMask) == PsrMusr;
	switch(ureg->type){
	default:
		panic("unknown trap");
		break;
	case PsrMabt:	/* prefetch fault */
		iprint("prefetch abort at %lux\n", ureg->pc-4);
		faultarm(ureg, ureg->pc - 4, user, 1);
		break;
	case PsrMabt+1:	/* data fault */
		pc = ureg->pc - 8;
		va = getfar();
		switch(getfsr() & 0xf){
		case 0x0:
			panic("vector exception at %lux\n", pc);
			break;
		case 0x1:
		case 0x3:
			if(user){
				snprint(buf, sizeof(buf), "sys: alignment: pc 0x%lux va 0x%lux\n",
					pc, va);
				postnote(up, 1, buf, NDebug);
			} else
				panic("kernel alignment: pc 0x%lux va 0x%lux", pc, va);
			break;
		case 0x2:
			panic("terminal exception at %lux\n", pc);
			break;
		case 0x4:
		case 0x6:
		case 0x8:
		case 0xa:
		case 0xc:
		case 0xe:
			panic("external abort at %lux\n", pc);
			break;
		case 0x5:
		case 0x7:
			/* translation fault, i.e., no pte entry */
			faultarm(ureg, va, user, 0);
			break;
		case 0x9:
		case 0xb:
			/* domain fault, accessing something we shouldn't */
			if(user){
				sprint(buf, "sys: access violation: pc 0x%lux va 0x%lux\n", pc, va);
				postnote(up, 1, buf, NDebug);
			} else
				panic("kernel access violation: pc 0x%lux va 0x%lux\n", pc, va);
			break;
		case 0xd:
		case 0xf:
			/* permission error, copy on write or real permission error */
			faultarm(ureg, va, user, 1);
			break;
		}
		break;
	case PsrMund:	/* undefined instruction */
		panic("undefined instruction");
		break;
	case PsrMirq:	/* device interrupt */
		for(i = 0; i < 32; i++)
			if((1<<i) & intrregs->icip){
				iprint("irq: %d\n", i);
				for(v = vctl[i]; v != nil; v = v->next)
					v->f(ureg, v->a);
			}
		break;
	}

	if(user && (up->procctl || up->nnote)){
		splhi();
		notify(ureg);
	}
}

/*
 *  system calls
 */
#include "../port/systab.h"

/*
 *  Syscall is called directly from assembler without going through trap().
 */
void
syscall(Ureg* ureg)
{
	ulong	sp;
	long	ret;
	int	i, scallnr;

	if((ureg->psr & PsrMask) != PsrMusr)
		panic("syscall: cs 0x%4.4uX\n", ureg->psr);

	m->syscall++;
	up->insyscall = 1;
	up->pc = ureg->pc;
	up->dbgreg = ureg;

	scallnr = ureg->r0;
	up->scallnr = scallnr;
	spllo();

	sp = ureg->sp;
	up->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(scallnr >= nsyscall){
			pprint("bad sys call number %d pc %lux\n",
				scallnr, ureg->pc);
			postnote(up, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}

		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-sizeof(Sargs)-BY2WD))
			validaddr(sp, sizeof(Sargs)+BY2WD, 0);

		up->s = *((Sargs*)(sp+BY2WD));
		up->psstate = sysctab[scallnr];

		ret = systab[scallnr](up->s.args);
		poperror();
	}
	if(up->nerrlab){
		print("bad errstack [%d]: %d extra\n", scallnr, up->nerrlab);
		for(i = 0; i < NERR; i++)
			print("sp=%lux pc=%lux\n",
				up->errlab[i].sp, up->errlab[i].pc);
		panic("error stack");
	}

	up->insyscall = 0;
	up->psstate = 0;

	/*
	 *  Put return value in frame.  On the x86 the syscall is
	 *  just another trap and the return value from syscall is
	 *  ignored.  On other machines the return value is put into
	 *  the results register by caller of syscall.
	 */
	ureg->r0 = ret;

	if(scallnr == NOTED)
		noted(ureg, *(ulong*)(sp+BY2WD));

	if(scallnr != RFORK && (up->procctl || up->nnote)){
		splhi();
		notify(ureg);
	}
}

/*
 *   Return user to state before notify()
 */
void
noted(Ureg* ureg, ulong arg0)
{
	Ureg *nureg;
	ulong oureg, sp;

	qlock(&up->debug);
	if(arg0!=NRSTR && !up->notified) {
		qunlock(&up->debug);
		pprint("call to noted() when not notified\n");
		pexit("Suicide", 0);
	}
	up->notified = 0;

	nureg = up->ureg;	/* pointer to user returned Ureg struct */

	/* sanity clause */
	oureg = (ulong)nureg;
	if(!okaddr((ulong)oureg-BY2WD, BY2WD+sizeof(Ureg), 0)){
		pprint("bad ureg in noted or call to noted when not notified\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	/* don't let user change system flags */
	nureg->psr = (ureg->psr & ~(PsrMask|PsrDfiq|PsrDirq)) |
			(nureg->psr & (PsrMask|PsrDfiq|PsrDirq));

	memmove(ureg, nureg, sizeof(Ureg));

	switch(arg0){
	case NCONT:
	case NRSTR:
		if(!okaddr(nureg->pc, 1, 0) || !okaddr(nureg->sp, BY2WD, 0)){
			qunlock(&up->debug);
			pprint("suicide: trap in noted\n");
			pexit("Suicide", 0);
		}
		up->ureg = (Ureg*)(*(ulong*)(oureg-BY2WD));
		qunlock(&up->debug);
		break;

	case NSAVE:
		if(!okaddr(nureg->pc, BY2WD, 0)
		|| !okaddr(nureg->sp, BY2WD, 0)){
			qunlock(&up->debug);
			pprint("suicide: trap in noted\n");
			pexit("Suicide", 0);
		}
		qunlock(&up->debug);
		sp = oureg-4*BY2WD-ERRLEN;
		splhi();
		ureg->sp = sp;
		((ulong*)sp)[1] = oureg;	/* arg 1 0(FP) is ureg* */
		((ulong*)sp)[0] = 0;		/* arg 0 is pc */
		break;

	default:
		pprint("unknown noted arg 0x%lux\n", arg0);
		up->lastnote.flag = NDebug;
		/* fall through */
		
	case NDFLT:
		if(up->lastnote.flag == NDebug){ 
			qunlock(&up->debug);
			pprint("suicide: %s\n", up->lastnote.msg);
		} else
			qunlock(&up->debug);
		pexit(up->lastnote.msg, up->lastnote.flag!=NDebug);
	}
}

/*
 *  Call user, if necessary, with note.
 *  Pass user the Ureg struct and the note on his stack.
 */
int
notify(Ureg* ureg)
{
	int l;
	ulong s, sp;
	Note *n;

	if(up->procctl)
		procctl(up);
	if(up->nnote == 0)
		return 0;

	s = spllo();
	qlock(&up->debug);
	up->notepending = 0;
	n = &up->note[0];
	if(strncmp(n->msg, "sys:", 4) == 0){
		l = strlen(n->msg);
		if(l > ERRLEN-15)	/* " pc=0x12345678\0" */
			l = ERRLEN-15;
		sprint(n->msg+l, " pc=0x%.8lux", ureg->pc);
	}

	if(n->flag!=NUser && (up->notified || up->notify==0)){
		if(n->flag == NDebug)
			pprint("suicide: %s\n", n->msg);
		qunlock(&up->debug);
		pexit(n->msg, n->flag!=NDebug);
	}

	if(up->notified) {
		qunlock(&up->debug);
		splhi();
		return 0;
	}
		
	if(!up->notify){
		qunlock(&up->debug);
		pexit(n->msg, n->flag!=NDebug);
	}
	sp = ureg->sp;
	sp -= sizeof(Ureg);

	if(!okaddr((ulong)up->notify, 1, 0)
	|| !okaddr(sp-ERRLEN-4*BY2WD, sizeof(Ureg)+ERRLEN+4*BY2WD, 1)){
		pprint("suicide: bad address in notify\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	up->ureg = (void*)sp;
	memmove((Ureg*)sp, ureg, sizeof(Ureg));
	*(Ureg**)(sp-BY2WD) = up->ureg;	/* word under Ureg is old up->ureg */
	up->ureg = (void*)sp;
	sp -= BY2WD+ERRLEN;
	memmove((char*)sp, up->note[0].msg, ERRLEN);
	sp -= 3*BY2WD;
	*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
	*(ulong*)(sp+1*BY2WD) = (ulong)up->ureg;	/* arg 1 is ureg* */
	*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
	ureg->sp = sp;
	ureg->pc = (ulong)up->notify;
	up->notified = 1;
	up->nnote--;
	memmove(&up->lastnote, &up->note[0], sizeof(Note));
	memmove(&up->note[0], &up->note[1], up->nnote*sizeof(Note));

	qunlock(&up->debug);
	splx(s);
	return 1;
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg *ureg, Proc *p)
{
	ureg->pc = p->sched.pc;
	ureg->sp = p->sched.sp+4;
}

/*
 *  return the userpc the last exception happened at
 */
ulong
userpc(void)
{
	Ureg *ureg;

	ureg = (Ureg*)up->dbgreg;
	return ureg->pc;
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
static void
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
	Ureg *cureg;

	p->sched.sp = (ulong)p->kstack+KSTACK-sizeof(Ureg);
	p->sched.pc = (ulong)forkret;

	cureg = (Ureg*)(p->sched.sp+2*BY2WD);
	memmove(cureg, ureg, sizeof(Ureg));

	/* syscall returns 0 for child */
	cureg->r0 = 0;

	/* Things from bottom of syscall which were never executed */
	p->psstate = 0;
	p->insyscall = 0;
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

/*
 *  pc output by ps
 */
ulong
dbgpc(Proc *p)
{
	Ureg *ureg;

	ureg = p->dbgreg;
	if(ureg == 0)
		return 0;

	return ureg->pc;
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


/*
 *  here on a hardware reset
 */
void
reset(void)
{
}
