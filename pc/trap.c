#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

void	noted(Ureg*, ulong);

static void debugbpt(Ureg*, void*);
static void fault386(Ureg*, void*);
static void syscall(Ureg*, void*);

static Lock irqctllock;
static Irqctl *irqctl[256];

void
intrenable(int v, void (*f)(Ureg*, void*), void* a, int tbdf)
{
	Irq * irq;
	Irqctl *ctl;

	lock(&irqctllock);
	if(irqctl[v] == 0){
		ctl = xalloc(sizeof(Irqctl));
		if(arch->intrenable(v, tbdf, ctl) == -1){
			unlock(&irqctllock);
			/*
			print("intrenable: didn't find v %d, tbdf 0x%uX\n", v, tbdf);
			 */
			xfree(ctl);
			return;
		}
		irqctl[v] = ctl;
	}
	ctl = irqctl[v];
	irq = xalloc(sizeof(Irq));
	irq->f = f;
	irq->a = a;
	irq->next = ctl->irq;
	ctl->irq = irq;
	unlock(&irqctllock);
}

void
trapinit(void)
{
	int v, pri;
	ulong vaddr;
	Segdesc *idt;

	idt = (Segdesc*)IDTADDR;
	vaddr = (ulong)vectortable;
	for(v = 0; v < 256; v++){
		if(v == VectorBPT || v == VectorSYSCALL)
			pri = 3;
		else
			pri = 0;
		idt[v].d0 = (vaddr & 0xFFFF)|(KESEL<<16);
		idt[v].d1 = (vaddr & 0xFFFF0000)|SEGP|SEGPL(pri)|SEGIG;
		vaddr += 6;
	}

	intrenable(VectorBPT, debugbpt, 0, BUSUNKNOWN);
	intrenable(VectorPF, fault386, 0, BUSUNKNOWN);
	intrenable(VectorSYSCALL, syscall, 0, BUSUNKNOWN);
}

char *excname[] = {
	[0]	"divide error",
	[1]	"debug exception",
	[2]	"nonmaskable interrupt",
	[3]	"breakpoint",
	[4]	"overflow",
	[5]	"bounds check",
	[6]	"invalid opcode",
	[7]	"coprocessor not available",
	[8]	"double fault",
	[9]	"coprocessor segment overrun",
	[10]	"invalid TSS",
	[11]	"segment not present",
	[12]	"stack exception",
	[13]	"general protection violation",
	[14]	"page fault",
	[15]	"15 (reserved)",
	[16]	"coprocessor error",
	[17]	"alignment check",
	[18]	"machine check",
};

static int nspuriousintr;

/*
 *  All traps come here.  It is slower to have all traps call trap() rather than
 *  directly vectoring the handler.  However, this avoids a lot of code duplication
 *  and possible bugs.  trap is called splhi().
 */
void
trap(Ureg* ureg)
{
	int v, user;
	char buf[ERRLEN];
	Irqctl *ctl;
	Irq *irq;

	user = (ureg->cs & 0xFFFF) == UESEL;
	if(user)
		up->dbgreg = ureg;

	v = ureg->trap;
	if(ctl = irqctl[v]){
		if(ctl->isintr)
			m->intr++;
		if(ctl->isr)
			ctl->isr(v);

		for(irq = ctl->irq; irq; irq = irq->next)
			irq->f(ureg, irq->a);

		if(ctl->eoi)
			ctl->eoi(v);
	}
	else if(v <= 16 && user){
		spllo();
		sprint(buf, "sys: trap: %s", excname[v]);
		postnote(up, 1, buf, NDebug);
	}
	else if(v >= VectorPIC && v <= MaxVectorPIC){
		/*
		 * An unknown interrupt.
		 * Check for a default IRQ7. This can happen when
		 * the IRQ input goes away before the acknowledge.
		 * In this case, a 'default IRQ7' is generated, but
		 * the corresponding bit in the ISR isn't set.
		 * In fact, just ignore all such interrupts.
		 */
		if(nspuriousintr < 2)
			print("spurious interrupt %d\n", v-VectorPIC);
		nspuriousintr++;
		return;
	}
	else{
		dumpregs(ureg);
		if(v < nelem(excname))
			panic("%s", excname[v]);
		panic("unknown trap/intr: %d\n", v);
	}

	/*
	 *  check user since syscall does its own notifying
	 */
	splhi();
	if(v != VectorSYSCALL && user && (up->procctl || up->nnote))
		notify(ureg);
}

/*
 *  dump registers
 */
void
dumpregs2(Ureg* ureg)
{
	ureg->cs &= 0xFFFF;
	ureg->ds &= 0xFFFF;
	ureg->es &= 0xFFFF;
	ureg->fs &= 0xFFFF;
	ureg->gs &= 0xFFFF;

	if(up)
		print("cpu%d: registers for %s %d\n", m->machno, up->text, up->pid);
	else
		print("cpu%d: registers for kernel\n", m->machno);
	print("FLAGS=%luX TRAP=%luX ECODE=%luX PC=%luX", ureg->flags, ureg->trap,
		ureg->ecode, ureg->pc);
	print(" SS=%4.4luX USP=%luX\n", ureg->ss & 0xFFFF, ureg->usp);
	print("  AX %8.8luX  BX %8.8luX  CX %8.8luX  DX %8.8luX\n",
		ureg->ax, ureg->bx, ureg->cx, ureg->dx);
	print("  SI %8.8luX  DI %8.8luX  BP %8.8luX\n",
		ureg->si, ureg->di, ureg->bp);
	print("  CS %4.4uX  DS %4.4uX  ES %4.4uX  FS %4.4uX  GS %4.4uX\n",
		ureg->cs, ureg->ds, ureg->es, ureg->fs, ureg->gs);
}

void
dumpregs(Ureg* ureg)
{
	extern ulong etext;
	ulong mca[2], mct[2];

	dumpregs2(ureg);

	/*
	 * Processor control registers.
	 * If machine check exception, time stamp counter, page size extensions or
	 * enhanced virtual 8086 mode extensions are supported, there is a CR4.
	 * If there is a CR4 and machine check extensions, read the machine check
	 * address and machine check type registers if RDMSR supported.
	 */
	print("  CR0 %8.8lux CR2 %8.8lux CR3 %8.8lux", getcr0(), getcr2(), getcr3());
	if(m->cpuiddx & 0x9A){
		print(" CR4 %8.8luX", getcr4());
		if((m->cpuiddx & 0xA0) == 0xA0){
			rdmsr(0x00, &mca[1], &mca[0]);
			rdmsr(0x01, &mct[1], &mct[0]);
			print("\n  MCA %8.8luX:%8.8luX MCT %8.8luX",
				mca[1], mca[0], mct[0]);
		}
	}
	print("\n  ur %luX up %luX\n", ureg, up);
}

void
dumpstack(void)
{
	ulong l, v, i;
	uchar *p;
	extern ulong etext;

	if(up == 0)
		return;

	i = 0;
	for(l=(ulong)&l; l<(ulong)(up->kstack+KSTACK); l+=4){
		v = *(ulong*)l;
		if(KTZERO < v && v < (ulong)&etext){
			p = (uchar*)v;
			if(*(p-5) == 0xE8){
				print("%lux ", p-5);
				i++;
			}
		}
		if(i == 8){
			i = 0;
			print("\n");
		}
	}
}

static void
debugbpt(Ureg* ureg, void*)
{
	char buf[ERRLEN];

	if(up == 0)
		panic("kernel bpt");
	/* restore pc to instruction that caused the trap */
	ureg->pc--;
	sprint(buf, "sys: breakpoint");
	postnote(up, 1, buf, NDebug);
}

static void
fault386(Ureg* ureg, void*)
{
	ulong addr;
	int read, user, n, insyscall;
	char buf[ERRLEN];

	insyscall = up->insyscall;
	up->insyscall = 1;
	addr = getcr2();
	read = !(ureg->ecode & 2);
	user = (ureg->cs&0xffff) == UESEL;
	spllo();
	n = fault(addr, read);
	if(n < 0){
		if(user){
			sprint(buf, "sys: trap: fault %s addr=0x%lux",
				read? "read" : "write", addr);
			postnote(up, 1, buf, NDebug);
			return;
		}
		dumpregs(ureg);
		panic("fault: 0x%lux\n", addr);
	}
	up->insyscall = insyscall;
}

/*
 *  system calls
 */
#include "../port/systab.h"

/*
 *  syscall is called splhi()
 */
static void
syscall(Ureg* ureg, void*)
{
	ulong	sp;
	long	ret;
	int	i;

	m->syscall++;
	up->insyscall = 1;
	up->pc = ureg->pc;
	up->dbgreg = ureg;

	if((ureg->cs)&0xffff == KESEL)
		panic("recursive system call");

	up->scallnr = ureg->ax;
	if(up->scallnr == RFORK && up->fpstate == FPactive){
		/*
		 *  so that the child starts out with the
		 *  same registers as the parent.
		 *  this must be atomic relative to this CPU, hence
		 *  the spl's.
		 */
		if(up->fpstate == FPactive){
			fpsave(&up->fpsave);
			up->fpstate = FPinactive;
		}
	}
	spllo();

	sp = ureg->usp;
	up->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(up->scallnr >= nsyscall){
			pprint("bad sys call number %d pc %lux\n", up->scallnr, ureg->pc);
			postnote(up, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}

		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-(1+MAXSYSARG)*BY2WD))
			validaddr(sp, (1+MAXSYSARG)*BY2WD, 0);

		up->s = *((Sargs*)(sp+1*BY2WD));
		up->psstate = sysctab[up->scallnr];

		ret = systab[up->scallnr](up->s.args);
		poperror();
	}
	if(up->nerrlab){
		print("bad errstack [%d]: %d extra\n", up->scallnr, up->nerrlab);
		for(i = 0; i < NERR; i++)
			print("sp=%lux pc=%lux\n", up->errlab[i].sp, up->errlab[i].pc);
		panic("error stack");
	}

	up->insyscall = 0;
	up->psstate = 0;

	/*
	 *  Put return value in frame.  On the safari the syscall is
	 *  just another trap and the return value from syscall is
	 *  ignored.  On other machines the return value is put into
	 *  the results register by caller of syscall.
	 */
	ureg->ax = ret;

	if(up->scallnr == NOTED)
		noted(ureg, *(ulong*)(sp+BY2WD));
	splhi();

	if(up->scallnr!=RFORK && (up->procctl || up->nnote))
		notify(ureg);

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
	sp = ureg->usp;
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
	ureg->usp = sp;
	ureg->pc = (ulong)up->notify;
	up->notified = 1;
	up->nnote--;
	memmove(&up->lastnote, &up->note[0], sizeof(Note));
	memmove(&up->note[0], &up->note[1], up->nnote*sizeof(Note));

	qunlock(&up->debug);
	splx(s);
	return 1;
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

	nureg = up->ureg;		/* pointer to user returned Ureg struct */

	/* sanity clause */
	oureg = (ulong)nureg;
	if(!okaddr((ulong)oureg-BY2WD, BY2WD+sizeof(Ureg), 0)){
		pprint("bad ureg in noted or call to noted() when not notified\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	/* don't let user change text or stack segments */
	nureg->cs = ureg->cs;
	nureg->ss = ureg->ss;

	/* don't let user change system flags */
	nureg->flags = (ureg->flags & ~0xCD5) | (nureg->flags & 0xCD5);

	memmove(ureg, nureg, sizeof(Ureg));

	switch(arg0){
	case NCONT:
	case NRSTR:
		if(!okaddr(nureg->pc, 1, 0) || !okaddr(nureg->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&up->debug);
			pexit("Suicide", 0);
		}
		up->ureg = (Ureg*)(*(ulong*)(oureg-BY2WD));
		qunlock(&up->debug);
		break;

	case NSAVE:
		if(!okaddr(nureg->pc, BY2WD, 0) || !okaddr(nureg->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&up->debug);
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
		if(up->lastnote.flag == NDebug)
			pprint("suicide: %s\n", up->lastnote.msg);
		qunlock(&up->debug);
		pexit(up->lastnote.msg, up->lastnote.flag!=NDebug);
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

ulong
userpc(void)
{
	Ureg *ureg;

	ureg = (Ureg*)up->dbgreg;
	return ureg->pc;
}

/* This routine must save the values of registers the user is not permitted to write
 * from devproc and then restore the saved values before returning
 */
void
setregisters(Ureg* ureg, char* pureg, char* uva, int n)
{
	ulong flags;
	ulong cs;
	ulong ss;

	flags = ureg->flags;
	cs = ureg->cs;
	ss = ureg->ss;
	memmove(pureg, uva, n);
	ureg->flags = (ureg->flags & 0x00FF) | (flags & 0xFF00);
	ureg->cs = cs;
	ureg->ss = ss;
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
	Ureg *cureg;

	/*
	 * Add 2*BY2WD to the stack to account for
	 *  - the return PC
	 *  - trap's argument (ur)
	 */
	p->sched.sp = (ulong)p->kstack+KSTACK-(sizeof(Ureg)+2*BY2WD);
	p->sched.pc = (ulong)forkret;

	cureg = (Ureg*)(p->sched.sp+2*BY2WD);
	memmove(cureg, ureg, sizeof(Ureg));
	cureg->ax = 0;				/* return value of syscall in child */

	/* Things from bottom of syscall which were never executed */
	p->psstate = 0;
	p->insyscall = 0;
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg* ureg, Proc* p)
{
	ureg->pc = p->sched.pc;
	ureg->sp = p->sched.sp+4;
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
