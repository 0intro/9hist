#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

void	noted(Ureg*, ulong);

void	intr0(void), intr1(void), intr2(void), intr3(void);
void	intr4(void), intr5(void), intr6(void), intr7(void);
void	intr8(void), intr9(void), intr10(void), intr11(void);
void	intr12(void), intr13(void), intr14(void), intr15(void);
void	intr16(void), intr17(void), intr18(void);
void	intr24(void), intr25(void), intr26(void), intr27(void);
void	intr28(void), intr29(void), intr30(void), intr31(void);
void	intr32(void), intr33(void), intr34(void), intr35(void);
void	intr36(void), intr37(void), intr38(void), intr39(void);
void	intr64(void);
void	intrbad(void);

/*
 *  8259 interrupt controllers
 */
enum
{
	Int0ctl=	0x20,		/* control port (ICW1, OCW2, OCW3) */
	Int0aux=	0x21,		/* everything else (ICW2, ICW3, ICW4, OCW1) */
	Int1ctl=	0xA0,		/* control port */
	Int1aux=	0xA1,		/* everything else (ICW2, ICW3, ICW4, OCW1) */

	Icw1=		0x10,		/* select bit in ctl register */
	Ocw2=		0x00,
	Ocw3=		0x08,

	EOI=		0x20,		/* non-specific end of interrupt */

	Maxhandler=	128,		/* max number of interrupt handlers */
};

int	int0mask = 0xff;	/* interrupts enabled for first 8259 */
int	int1mask = 0xff;	/* interrupts enabled for second 8259 */

/*
 *  trap/interrupt gates
 */
Segdesc ilt[256];
int badintr[16];

typedef struct Handler	Handler;
struct Handler
{
	void	(*r)(Ureg*, void*);
	void	*arg;
	Handler	*next;
};

struct
{
	Lock;
	Handler	*ivec[256];
	Handler	h[Maxhandler];
	int	free;
} halloc;

void
sethvec(int v, void (*r)(void), int type, int pri)
{
	ilt[v].d0 = ((ulong)r)&0xFFFF|(KESEL<<16);
	ilt[v].d1 = ((ulong)r)&0xFFFF0000|SEGP|SEGPL(pri)|type;
}

void
setvec(int v, void (*r)(Ureg*, void*), void *arg)
{
	Handler *h;

	lock(&halloc);
	if(halloc.free >= Maxhandler)
		panic("out of interrupt handlers");
	h = &halloc.h[halloc.free++];
	h->next = halloc.ivec[v];
	h->r = r;
	h->arg = arg;
	halloc.ivec[v] = h;
	unlock(&halloc);

	/*
	 *  enable corresponding interrupt in 8259
	 */
	if((v&~0x7) == Int0vec){
		int0mask &= ~(1<<(v&7));
		outb(Int0aux, int0mask);
	} else if((v&~0x7) == Int1vec){
		int1mask &= ~(1<<(v&7));
		outb(Int1aux, int1mask);
	}
}

void
debugbpt(Ureg *ur, void *arg)
{
	char buf[ERRLEN];

	USED(arg);

	if(up == 0)
		panic("kernel bpt");
	/* restore pc to instruction that caused the trap */
	ur->pc--;
	sprint(buf, "sys: breakpoint");
	postnote(up, 1, buf, NDebug);
}

/*
 *  set up the interrupt/trap gates
 */
void
trapinit(void)
{
	int i;

	/*
	 *  set all interrupts to panics
	 */
	for(i = 0; i < 256; i++)
		sethvec(i, intrbad, SEGIG, 0);

	/*
	 *  80386 processor (and coprocessor) traps
	 */
	sethvec(0, intr0, SEGIG, 0);
	sethvec(1, intr1, SEGIG, 0);
	sethvec(2, intr2, SEGIG, 0);
	sethvec(4, intr4, SEGIG, 0);
	sethvec(5, intr5, SEGIG, 0);
	sethvec(6, intr6, SEGIG, 0);
	sethvec(7, intr7, SEGIG, 0);
	sethvec(8, intr8, SEGIG, 0);
	sethvec(9, intr9, SEGIG, 0);
	sethvec(10, intr10, SEGIG, 0);
	sethvec(11, intr11, SEGIG, 0);
	sethvec(12, intr12, SEGIG, 0);
	sethvec(13, intr13, SEGIG, 0);
	sethvec(14, intr14, SEGIG, 0);	/* page fault */
	sethvec(15, intr15, SEGIG, 0);
	sethvec(16, intr16, SEGIG, 0);	/* math coprocessor */
	sethvec(17, intr17, SEGIG, 0);
	sethvec(18, intr18, SEGIG, 0);

	/*
	 *  device interrupts
	 */
	sethvec(24, intr24, SEGIG, 0);
	sethvec(25, intr25, SEGIG, 0);
	sethvec(26, intr26, SEGIG, 0);
	sethvec(27, intr27, SEGIG, 0);
	sethvec(28, intr28, SEGIG, 0);
	sethvec(29, intr29, SEGIG, 0);
	sethvec(30, intr30, SEGIG, 0);
	sethvec(31, intr31, SEGIG, 0);
	sethvec(32, intr32, SEGIG, 0);
	sethvec(33, intr33, SEGIG, 0);
	sethvec(34, intr34, SEGIG, 0);
	sethvec(35, intr35, SEGIG, 0);
	sethvec(36, intr36, SEGIG, 0);
	sethvec(37, intr37, SEGIG, 0);
	sethvec(38, intr38, SEGIG, 0);
	sethvec(39, intr39, SEGIG, 0);

	/*
	 *  system calls and break points
	 */
	sethvec(Syscallvec, intr64, SEGIG, 3);
	setvec(Syscallvec, syscall, 0);
	sethvec(Bptvec, intr3, SEGIG, 3);
	setvec(Bptvec, debugbpt, 0);


	/*
	 *  tell the hardware where the table is (and how long)
	 */
	putidt(ilt, sizeof(ilt));

	/*
	 *  Set up the first 8259 interrupt processor.
	 *  Make 8259 interrupts start at CPU vector Int0vec.
	 *  Set the 8259 as master with edge triggered
	 *  input with fully nested interrupts.
	 */
	outb(Int0ctl, (1<<4)|(0<<3)|(1<<0));	/* ICW1 - master, edge triggered,
					  	 ICW4 will be sent */
	outb(Int0aux, Int0vec);		/* ICW2 - interrupt vector offset */
	outb(Int0aux, 0x04);		/* ICW3 - have slave on level 2 */
	outb(Int0aux, 0x01);		/* ICW4 - 8086 mode, not buffered */

	/*
	 *  Set up the second 8259 interrupt processor.
	 *  Make 8259 interrupts start at CPU vector Int1vec.
	 *  Set the 8259 as master with level triggered
	 *  input with fully nested interrupts.
	 */
	outb(Int1ctl, (1<<4)|(0<<3)|(1<<0));	/* ICW1 - master, level triggered,
					  	 ICW4 will be sent */
	outb(Int1aux, Int1vec);		/* ICW2 - interrupt vector offset */
	outb(Int1aux, 0x02);		/* ICW3 - I am a slave on level 2 */
	outb(Int1aux, 0x01);		/* ICW4 - 8086 mode, not buffered */

	/*
	 *  pass #2 8259 interrupts to #1
	 */
	int0mask &= ~0x04;
	outb(Int0aux, int0mask);

	/*
	 * Set Ocw3 to return the ISR when ctl read.
	 */
	outb(Int0ctl, Ocw3|0x03);
	outb(Int1ctl, Ocw3|0x03);
}

char *excname[] = {
	[0]	"divide error",
	[1]	"debug exception",
	[2]	" nonmaskable interrupt",
	[3]	"breakpoint",
	[4]	"overflow",
	[5]	"bounds check",
	[6]	"invalid opcode",
	[7]	"coprocessor not available",
	[8]	"double fault",
	[9]	"9 (reserved)",
	[10]	"invalid TSS",
	[11]	"segment not present",
	[12]	"stack exception",
	[13]	"general protection violation",
	[14]	"page fault",
	[15]	"15 (reserved)",
	[16]	"coprocessor error",
	[17]	"alignment check",
	[18]	"something bad happened",
};

Ureg lastur;
Ureg scndlastur;

/*
 *  All traps come here.  It is slower to have all traps call trap() rather than
 *  directly vectoring the handler.  However, this avoids a lot of code duplication
 *  and possible bugs.  trap is called splhi().
 */
void
trap(Ureg *ur)
{
	int v, user;
	int c;
	char buf[ERRLEN];
	Handler *h;
	static int iret_traps;
	ushort isr;

	v = ur->trap;

	user = (ur->cs&0xffff) == UESEL;
	if(user)
		up->dbgreg = ur;
	else if(ur->pc <= (ulong)end && *(uchar*)ur->pc == 0xCF) {
		if(iret_traps++ > 10)
			panic("iret trap");
		goto out;
	}
	iret_traps = 0;

	/*
	 *  tell the 8259 that we're done with the
	 *  highest level interrupt (interrupts are still
	 *  off at this point)
	 */
	c = v&~0x7;
	isr = 0;
	if(c==Int0vec || c==Int1vec){
		isr = inb(Int0ctl);
		outb(Int0ctl, EOI);
		if(c == Int1vec){
			isr |= inb(Int1ctl)<<8;
			outb(Int1ctl, EOI);
		}
	}

	if(v>=256 || (h = halloc.ivec[v]) == 0){
		/* an old 386 generates these fairly often, no idea why */
		if(v == 13)
			goto out;

		/* a processor or coprocessor error */
		if(v <= 16){
			if(user){
				sprint(buf, "sys: trap: %s", excname[v]);
				postnote(up, 1, buf, NDebug);
				goto out;
			} else {
				dumpregs(ur);
print("%s pc=0x%lux", excname[v], ur->pc);
for(;;);
				panic("%s pc=0x%lux", excname[v], ur->pc);
			}
		}

		if(v >= Int0vec && v < Int0vec+16){
			/* an unknown interrupt */
			v -= Int0vec;
			/*
			 * Check for a default IRQ7. This can happen when
			 * the IRQ input goes away before the acknowledge.
			 * In this case, a 'default IRQ7' is generated, but
			 * the corresponding bit in the ISR isn't set.
			 * In fact, just ignore all such interrupts.
			 */
			if((isr & (1<<v)) == 0)
				goto out;
			if(badintr[v]++ == 0 || (badintr[v]%100000) == 0){
				print("unknown interrupt %d pc=0x%lux: total %d\n", v,
					ur->pc, badintr[v]);
				print("isr = 0x%4.4ux\n", isr);
			}
		} else {
			/* unimplemented traps */
			print("illegal trap %d pc=0x%lux\n", v, ur->pc);
		}
		goto out;
	}

	/* there may be multiple handlers on one interrupt level */
	do {
		(*h->r)(ur, h->arg);
		h = h->next;
	} while(h);

	/*
	 *  check user since syscall does its own notifying
	 */
	splhi();
	if(v != Syscallvec && user && (up->procctl || up->nnote))
		notify(ur);
out:
	scndlastur = lastur;
	lastur = *ur;
}

/*
 *  dump registers
 */
void
dumpregs2(Ureg *ur)
{
	ur->cs &= 0xffff;
	ur->ds &= 0xffff;
	ur->es &= 0xffff;
	ur->fs &= 0xffff;
	ur->gs &= 0xffff;

	if(up)
		print("registers for %s %d\n", up->text, up->pid);
	else
		print("registers for kernel\n");
	print("FLAGS=%lux TRAP=%lux ECODE=%lux CS=%4.4lux PC=%lux", ur->flags, ur->trap,
		ur->ecode, ur->cs, ur->pc);
	print(" SS=%4.4lux USP=%lux\n", ur->ss&0xffff, ur->usp);
	print("  AX %8.8lux  BX %8.8lux  CX %8.8lux  DX %8.8lux\n",
		ur->ax, ur->bx, ur->cx, ur->dx);
	print("  SI %8.8lux  DI %8.8lux  BP %8.8lux\n",
		ur->si, ur->di, ur->bp);
	print("  DS %4.4lux  ES %4.4lux  FS %4.4lux  GS %4.4lux\n",
		ur->ds, ur->es, ur->fs, ur->gs);
}

void
dumpregs(Ureg *ur)
{
	extern ulong etext;
	ulong *x;


	x = (ulong*)(ur+1);
	dumpregs2(&scndlastur);
	dumpregs2(&lastur);
	dumpregs2(ur);
	print("  CR0 %8.8lux CR2 %8.8lux\n", getcr0(), getcr2());
	print("  magic %lux %lux %lux\n", x[0], x[1], x[2]);
	print("  ur %lux up %lux\n", ur, up);
}

void
dumpstack(void)
{
	ulong l, v, i;
	extern ulong etext;

	if(up == 0)
		return;

	i = 0;
	for(l=(ulong)&l; l<(ulong)(up->kstack+KSTACK); l+=4){
		v = *(ulong*)l;
		if(KTZERO < v && v < (ulong)&etext){
			print("%lux ", v);
			i++;
		}
		if(i == 8){
			i = 0;
			print("\n");
		}
	}
}

/*
 *  system calls
 */
#include "../port/systab.h"

/*
 *  syscall is called splhi()
 */
void
syscall(Ureg *ur, void *arg)
{
	ulong	sp;
	long	ret;
	int	i;

	USED(arg);


	up->insyscall = 1;
	up->pc = ur->pc;
	up->dbgreg = ur;

	if((ur->cs)&0xffff == KESEL)
		panic("recursive system call");

	up->scallnr = ur->ax;
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

	sp = ur->usp;
	up->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(up->scallnr >= nsyscall){
			pprint("bad sys call number %d pc %lux\n", up->scallnr, ur->pc);
			postnote(up, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}

		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-(1+MAXSYSARG)*BY2WD))
			validaddr(sp, (1+MAXSYSARG)*BY2WD, 0);

		up->s = *((Sargs*)(sp+1*BY2WD));
		up->psstate = sysctab[up->scallnr];

		ret = (*systab[up->scallnr])(up->s.args);
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
	ur->ax = ret;

	if(up->scallnr == NOTED)
		noted(ur, *(ulong*)(sp+BY2WD));
	splhi();

	if(up->scallnr!=RFORK && (up->procctl || up->nnote))
		notify(ur);

}

/*
 *  Call user, if necessary, with note.
 *  Pass user the Ureg struct and the note on his stack.
 */
int
notify(Ureg *ur)
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
		sprint(n->msg+l, " pc=0x%.8lux", ur->pc);
	}

	if(n->flag!=NUser && (up->notified || up->notify==0)){
		qunlock(&up->debug);
		if(n->flag == NDebug)
			pprint("suicide: %s\n", n->msg);
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
	sp = ur->usp;
	sp -= sizeof(Ureg);

	if(!okaddr((ulong)up->notify, 1, 0)
	|| !okaddr(sp-ERRLEN-4*BY2WD, sizeof(Ureg)+ERRLEN+4*BY2WD, 1)){
		pprint("suicide: bad address in notify\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	up->ureg = (void*)sp;
	memmove((Ureg*)sp, ur, sizeof(Ureg));
	*(Ureg**)(sp-BY2WD) = up->ureg;	/* word under Ureg is old up->ureg */
	up->ureg = (void*)sp;
	sp -= BY2WD+ERRLEN;
	memmove((char*)sp, up->note[0].msg, ERRLEN);
	sp -= 3*BY2WD;
	*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
	*(ulong*)(sp+1*BY2WD) = (ulong)up->ureg;	/* arg 1 is ureg* */
	*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
	ur->usp = sp;
	ur->pc = (ulong)up->notify;
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
noted(Ureg *ur, ulong arg0)
{
	Ureg *nur;
	ulong oureg, sp;

	qlock(&up->debug);
	if(arg0!=NRSTR && !up->notified) {
		qunlock(&up->debug);
		pprint("call to noted() when not notified\n");
		pexit("Suicide", 0);
	}
	up->notified = 0;

	nur = up->ureg;		/* pointer to user returned Ureg struct */

	/* sanity clause */
	oureg = (ulong)nur;
	if(!okaddr((ulong)oureg-BY2WD, BY2WD+sizeof(Ureg), 0)){
		pprint("bad ureg in noted or call to noted() when not notified\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	/* don't let user change text or stack segments */
	nur->cs = ur->cs;
	nur->ss = ur->ss;

	/* don't let user change system flags */
	nur->flags = (ur->flags & ~0xCD5) | (nur->flags & 0xCD5);

	memmove(ur, nur, sizeof(Ureg));

	switch(arg0){
	case NCONT:
	case NRSTR:
		if(!okaddr(nur->pc, 1, 0) || !okaddr(nur->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&up->debug);
			pexit("Suicide", 0);
		}
		up->ureg = (Ureg*)(*(ulong*)(oureg-BY2WD));
		qunlock(&up->debug);
		break;

	case NSAVE:
		if(!okaddr(nur->pc, BY2WD, 0) || !okaddr(nur->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&up->debug);
			pexit("Suicide", 0);
		}
		qunlock(&up->debug);
		sp = oureg-4*BY2WD-ERRLEN;
		splhi();
		ur->sp = sp;
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
	Ureg *ur;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;

	ur = up->dbgreg;
	ur->usp = (ulong)sp;
	ur->pc = entry;
	return USTKTOP-BY2WD;		/* address of user-level clock */
}

ulong
userpc(void)
{
	Ureg *ur;

	ur = (Ureg*)up->dbgreg;
	return ur->pc;
}

/* This routine must save the values of registers the user is not permitted to write
 * from devproc and the restore the saved values before returning
 */
void
setregisters(Ureg *xp, char *pureg, char *uva, int n)
{
	ulong flags;
	ulong cs;
	ulong ss;

	flags = xp->flags;
	cs = xp->cs;
	ss = xp->ss;
	memmove(pureg, uva, n);
	xp->flags = (xp->flags & 0xff) | (flags & 0xff00);
	xp->cs = cs;
	xp->ss = ss;
}

static void
linkproc(void)
{
	spllo();
	(*up->kpfun)(up->kparg);
}

void
kprocchild(Proc *p, void (*func)(void*), void *arg)
{
	p->sched.pc = (ulong)linkproc;
	p->sched.sp = (ulong)p->kstack+KSTACK;

	p->kpfun = func;
	p->kparg = arg;
}

void
forkchild(Proc *p, Ureg *ur)
{
	Ureg *cur;

	/*
	 * We add 2*BY2Wd to the stack because we have to account for
	 *  - the return PC
	 *  - trap's argument (ur)
	 */
	p->sched.sp = (ulong)p->kstack+KSTACK-(sizeof(Ureg)+2*BY2WD);
	p->sched.pc = (ulong)forkret;

	cur = (Ureg*)(p->sched.sp+2*BY2WD);
	memmove(cur, ur, sizeof(Ureg));
	cur->ax = 0;				/* return value of syscall in child */

	/* Things from bottom of syscall we never got to execute */
	p->psstate = 0;
	p->insyscall = 0;
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg *xp, Proc *p)
{
	xp->pc = p->sched.pc;
	xp->sp = p->sched.sp+4;
}

ulong
dbgpc(Proc *p)
{
	Ureg *ur;

	ur = p->dbgreg;
	if(ur == 0)
		return 0;

	return ur->pc;
}
