#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"../port/error.h"

void	(*vmevec[256])(int);
void	noted(Ureg**, ulong);
void	rfnote(Ureg**);

char *excname[] =
{
	"trap: external interrupt",
	"trap: TLB modification",
	"trap: TLB miss (load or fetch)",
	"trap: TLB miss (store)",
	"trap: address error (load or fetch)",
	"trap: address error (store)",
	"trap: bus error (fetch)",
	"trap: bus error (data load or store)",
	"trap: system call",
	"breakpoint",
	"trap: reserved instruction",
	"trap: coprocessor unusable",
	"trap: arithmetic overflow",
	"trap: undefined 13",
	"trap: undefined 14",
	"trap: undefined 15",		/* used as sys call for debugger */
};

char *fpcause[] =
{
	"inexact operation",
	"underflow",
	"overflow",
	"division by zero",
	"invalid operation",
};
char	*fpexcname(Ureg*, ulong, char*);
#define FPEXPMASK	(0x3f<<12)	/* Floating exception bits in fcr31 */

char*
regname[]=
{
	"STATUS", "PC",	"SP", "CAUSE",
	"BADADDR", "TLBVIRT", "HI", "LO",
	"R31",	"R30",	"R28",	"R27",
	"R26",	"R25",	"R24",	"R23",
	"R22",	"R21",	"R20",	"R19",
	"R18",	"R17",	"R16",	"R15",
	"R14",	"R13",	"R12",	"R11",
	"R10",	"R9",	"R8",	"R7",
	"R6",	"R5",	"R4",	"R3",
	"R2",	"R1"
};

void
trap(Ureg *ur)
{
	ulong fcr31;
	int user, cop, x, fpchk, ecode;
	char buf[2*ERRLEN], buf1[ERRLEN], *fpexcep;

	user = ur->status&KUP;
	ecode = (ur->cause>>2)&0xf;

	fpchk = 0;
	if(user) {
		up->dbgreg = ur;
		if(up->fpstate == FPactive) {
			if((ur->status&CU1) == 0)		/* Paranoid */
				panic("FPactive but no CU1");
			up->fpstate = FPinactive;
			ur->status &= ~CU1;
			savefpregs(&up->fpsave);
			fptrap(ur);
			fpchk = 1;
		}
	}

	switch(ecode){
	case CINT:
		if(ur->cause&INTR3) {			/* FP trap */
			clrfpintr();
			ur->cause &= ~INTR3;
		}
		intr(ur);
		break;

	case CTLBM:
	case CTLBL:
	case CTLBS:
		if(up == 0)
			panic("kfault pc %lux addr %lux", ur->pc, ur->badvaddr);

		x = up->insyscall;
		up->insyscall = 1;

		spllo();
		faultmips(ur, user, ecode);
		up->insyscall = x;
		break;

	case CCPU:
		cop = (ur->cause>>28)&3;
		if(user && cop == 1) {
			if(up->fpstate == FPinit) {
				up->fpstate = FPinactive;
				fcr31 = up->fpsave.fpstatus;
				up->fpsave = initfp;
				up->fpsave.fpstatus = fcr31;
				break;
			}
			if(up->fpstate == FPinactive)
				break;
		}
		/* Fallthrough */

	default:
		if(user) {
			spllo();
			sprint(buf, "sys: %s", excname[ecode]);
			postnote(up, 1, buf, NDebug);
			break;
		}
		print("kernel %s pc=%lux\n", excname[ecode], ur->pc);
		dumpregs(ur);
		dumpstack();
		if(m->machno == 0)
			spllo();
		exit(1);
	}

	if(fpchk) {
		fcr31 = up->fpsave.fpstatus;
		if((fcr31>>12) & ((fcr31>>7)|0x20) & 0x3f) {
			spllo();
			fpexcep	= fpexcname(ur, fcr31, buf1);
			sprint(buf, "sys: fp: %s", fpexcep);
			postnote(up, 1, buf, NDebug);
		}
	}

	splhi();
	if(!user)
		return;

	notify(ur);
	if(up->fpstate == FPinactive) {
		restfpregs(&up->fpsave, up->fpsave.fpstatus);
		up->fpstate = FPactive;
		ur->status |= CU1;
	}
}

void
intr(Ureg *ur)
{
	long v;
	int i, any;
	ulong cause;
	static int bogies;
	uchar pend, xxx;

	m->intr++;
	cause = ur->cause&(INTR5|INTR4|INTR3|INTR2|INTR1);

	if(cause & INTR1){
		duartintr();
		cause &= ~INTR1;
	}

	while(cause & INTR5) {
		any = 0;
		if(!(*MPBERR1 & (1<<8))){
			print("MP bus error %lux %lux\n", *MPBERR0, *MPBERR1);
			print("PC %lux R31 %lux\n", ur->pc, ur->r31);
			*MPBERR0 = 0;
			i = *SBEADDR;
			USED(i);
			any = 1;
		}

		/*
		 *  directions from IO2 manual
		 *  1. clear all IO2 masks
		 */
		*IO2CLRMASK = 0xff000000;

		/*
		 *  2. wait for interrupt in progress
		 */
		while(!(*INTPENDREG & (1<<5)))
			;

		/*
		 *  3. read pending interrupts
		 */
		pend = SBCCREG->fintpending;

		/*
		 *  4. clear pending register
		 */
		i = SBCCREG->flevel;
		USED(i);

		/*
		 *  4a. attempt to fix problem
		 */
		if(!(*INTPENDREG & (1<<5)))
			print("pause again\n");
		while(!(*INTPENDREG & (1<<5)))
			;
		xxx = SBCCREG->fintpending;
		if(xxx){
			print("new pend %ux\n", xxx);
			i = SBCCREG->flevel;
			USED(i);
		}

		/*
		 *  5a. process lance, scsi
		 */
		if(pend & 1) {
			v = INTVECREG->i[0].vec;
			if(!(v & (1<<12))){
				print("ioberr %lux %lux\n", *MPBERR0, *MPBERR1);
				print("PC %lux R31 %lux\n", ur->pc, ur->r31);
				*MPBERR0 = 0;
				any = 1;
			}
			if(ioid < IO3R1){
				if(!(v & 7))
					any = 1;
				if(!(v & (1<<2)))
					lanceintr();
				if(!(v & (1<<1)))
					lanceparity();
				if(!(v & (1<<0)))
					print("SCSI interrupt\n");
			}else{
				if(v & 7)
					any = 1;
				if(v & (1<<2))
					lanceintr();
/*
				if(v & (1<<1))
					scsiintr(1);
				if(v & (1<<0))
					scsiintr(0);
*/
			}
		}
		/*
		 *  5b. process vme
		 *  i can guess your level
		 */
		for(i=1; pend>>=1; i++){
			if(pend & 1) {
				v = INTVECREG->i[i].vec;
				if(!(v & (1<<12))){
					print("io2 mp bus error %d %lux %lux\n",
						i, *MPBERR0, *MPBERR1);
					*MPBERR0 = 0;
				}
				v &= 0xff;
				(*vmevec[v])(v);
				any = 1;
			}
		}
		/*
		 *  6. re-enable interrupts
		 */
		*IO2SETMASK = 0xff000000;
		if(any == 0)
			cause &= ~INTR5;
	}

	if(cause & (INTR2|INTR4)) {
		LEDON(LEDclock);
		clock(ur);
		LEDOFF(LEDclock);
		cause &= ~(INTR2|INTR4);
	}

	if(cause)
		panic("cause %lux %lux\n", up, cause);
}

char*
fpexcname(Ureg *ur, ulong fcr31, char *buf)
{
	int i;
	char *s;
	ulong fppc;

	fppc = ur->pc;
	if(ur->cause & (1<<31))	/* branch delay */
		fppc += 4;
	s = 0;
	if(fcr31 & (1<<17))
		s = "unimplemented operation";
	else{
		fcr31 >>= 7;		/* trap enable bits */
		fcr31 &= (fcr31>>5);	/* anded with exceptions */
		for(i=0; i<5; i++)
			if(fcr31 & (1<<i))
				s = fpcause[i];
	}
	if(s == 0)
		return "no floating point exception";

	sprint(buf, "%s fppc=0x%lux", s, fppc);
	return buf;
}

void
dumpstack(void)
{
	ulong l, v, top;
	extern ulong etext;

	if(up == 0)
		return;

	top = (ulong)up->kstack + KSTACK;
	for(l=(ulong)&l; l < top; l += BY2WD) {
		v = *(ulong*)l;
		if(KTZERO < v && v < (ulong)&etext) {
			print("%lux=%lux\n", l, v);
			delay(100);
		}
	}
}

void
dumpregs(Ureg *ur)
{
	int i;
	ulong *l;

	if(up)
		print("registers for %s %d\n", up->text, up->pid);
	else
		print("registers for kernel\n");

	l = &ur->status;
	for(i=0; i<sizeof regname/sizeof(char*); i+=2) {
		print("%s\t%.8lux\t%s\t%.8lux\n",
			regname[i], l[0], regname[i+1], l[1]);
		l += 2;
		prflush();
	}
}

int
notify(Ureg *ur)
{
	int l;
	ulong sp;
	Note *n;

	if(up->procctl)
		procctl(up);
	if(up->nnote == 0)
		return 0;

	spllo();
	qlock(&up->debug);
	up->notepending = 0;
	n = &up->note[0];
	if(strncmp(n->msg, "sys:", 4) == 0) {
		l = strlen(n->msg);
		if(l > ERRLEN-15)	/* " pc=0x12345678\0" */
			l = ERRLEN-15;

		sprint(n->msg+l, " pc=0x%lux", ur->pc);
	}

	if(n->flag != NUser && (up->notified || up->notify==0)) {
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
		
	if(!up->notify) {
		qunlock(&up->debug);
		pexit(n->msg, n->flag!=NDebug);
	}
	up->svstatus = ur->status;
	sp = ur->usp - sizeof(Ureg);

	if(sp&0x3 || !okaddr((ulong)up->notify, BY2WD, 0)
	|| !okaddr(sp-ERRLEN-3*BY2WD, sizeof(Ureg)+ERRLEN+3*BY2WD, 1)) {
		pprint("suicide: bad address or sp in notify\n");
		qunlock(&up->debug);
		pexit("Suicide", 0);
	}

	up->ureg = (void*)sp;
	memmove((Ureg*)sp, ur, sizeof(Ureg));
	sp -= ERRLEN;
	memmove((char*)sp, up->note[0].msg, ERRLEN);
	sp -= 3*BY2WD;
	*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
	up->svr1 = ur->r1;			/* save away r1 */
	ur->r1 = (ulong)up->ureg;		/* arg 1 is ureg* */
	*(ulong*)(sp+1*BY2WD) = (ulong)up->ureg;/* arg 1 0(FP) is ureg* (for Alef) */
	*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
	ur->usp = sp;
	ur->pc = (ulong)up->notify;
	up->notified = 1;
	up->nnote--;
	memmove(&up->lastnote, &up->note[0], sizeof(Note));
	memmove(&up->note[0], &up->note[1], up->nnote*sizeof(Note));

	qunlock(&up->debug);
	splhi();
	return 1;
}

/*
 * Return user to state before notify()
 */
void
noted(Ureg **urp, ulong arg0)
{
	Ureg *nur;

	qlock(&up->debug);
	if(!up->notified) {
		qunlock(&up->debug);
		pprint("call to noted() when not notified\n");
		pexit("Suicide", 0);
	}
	up->notified = 0;

	nur = up->ureg;
	if(nur->status != up->svstatus) {
		qunlock(&up->debug);
		pprint("bad noted ureg status %lux\n", nur->status);
		pexit("Suicide", 0);
	}

	memmove(*urp, up->ureg, sizeof(Ureg));
	(*urp)->r1 = up->svr1;
	switch(arg0) {
	case NCONT:
		if(!okaddr(nur->pc, 1, 0) || !okaddr(nur->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&up->debug);
			pexit("Suicide", 0);
		}
		splhi();
		qunlock(&up->debug);
		rfnote(urp);
		break;

	default:
		pprint("unknown noted arg 0x%lux\n", arg0);
		up->lastnote.flag = NDebug;
		/* fall through */
		
	case NDFLT:
		if(up->lastnote.flag == NDebug)
			pprint("suicide: %s\n", up->lastnote.msg);
		qunlock(&up->debug);
		pexit(up->lastnote.msg, up->lastnote.flag != NDebug);
	}
}


#include "../port/systab.h"

long
syscall(Ureg *aur)
{
	long ret;
	ulong sp;
	Ureg *ur;

	m->syscall++;
	up->insyscall = 1;
	ur = aur;
	up->pc = ur->pc;
	up->dbgreg = aur;
	ur->cause = 15<<2;	/* for debugging: system call is undef 15; */

	if(up->fpstate == FPactive) {
		if((ur->status&CU1) == 0)
			panic("syscall: FPactive but no CU1");
		up->fpsave.fpstatus = fcr31();
		up->fpstate = FPinit;
		ur->status &= ~CU1;
	}

	spllo();

	if(up->procctl)
		procctl(up);

	up->scallnr = ur->r1;
	up->nerrlab = 0;
	sp = ur->sp;
	ret = -1;
	if(!waserror()){
		if(up->scallnr >= nsyscall) {
			pprint("bad sys call number %d pc %lux\n",
						up->scallnr, ur->pc);
			postnote(up, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}

		if(sp & (BY2WD-1)){
			pprint("odd sp in sys call pc %lux sp %lux\n", ur->pc, ur->sp);
			postnote(up, 1, "sys: odd stack", NDebug);
			error(Ebadarg);
		}

		if(sp < (USTKTOP-BY2PG) || sp > (USTKTOP-sizeof(Sargs)))
			validaddr(sp, sizeof(Sargs), 0);

		up->s = *((Sargs*)(sp+BY2WD));
		up->psstate = sysctab[up->scallnr];

		ret = (*systab[up->scallnr])(up->s.args);
		poperror();
	}
	ur->pc += 4;
	up->nerrlab = 0;
	up->psstate = 0;
	up->insyscall = 0;
	if(up->scallnr == NOTED)			/* ugly hack */
		noted(&aur, *(ulong*)(sp+BY2WD));	/* doesn't return */

	splhi();
	if(up->scallnr!=RFORK && (up->procctl || up->nnote)){
		ur->r1 = ret;			/* load up for noted() */
		if(notify(ur))
			return ur->r1;
	}

	return ret;
}

void
forkchild(Proc *p, Ureg *ur)
{
	Ureg *cur;

	p->sched.sp = (ulong)p->kstack+KSTACK-(sizeof(Ureg)+2*BY2WD);
	p->sched.pc = (ulong)forkret;

	cur = (Ureg*)(p->sched.sp+2*BY2WD);
	memmove(cur, ur, sizeof(Ureg));

	cur->pc += 4;

	/* Things from bottom of syscall we never got to execute */
	p->psstate = 0;
	p->insyscall = 0;
}

static
void
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

long
execregs(ulong entry, ulong ssize, ulong nargs)
{
	Ureg *ur;
	ulong *sp;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;

	ur = (Ureg*)up->dbgreg;
	ur->usp = (ulong)sp;
	ur->pc = entry - 4;			/* syscall advances it */
	up->fpsave.fpstatus = initfp.fpstatus;
	return USTKTOP-BY2WD;			/* address of user-level clock */
}

ulong
userpc(void)
{
	Ureg *ur;

	ur = (Ureg*)up->dbgreg;
	return ur->pc;
}

void
novme(int v)
{
	static count = 0;

	print("vme intr 0x%.2x\n", v);
	count++;
	if(count >= 10)
		panic("too many vme intr");
}

void
setvmevec(int v, void (*f)(int))
{
	void (*g)(int);

	v &= 0xff;
	g = vmevec[v];
	if(g && g != novme && g != f)
		print("second setvmevec to 0x%.2x\n", v);
	vmevec[v] = f;
}

/* This routine must save the values of registers the user is not permitted to
 * write from devproc and then restore the saved values before returning
 */
void
setregisters(Ureg *xp, char *pureg, char *uva, int n)
{
	ulong status;

	status = xp->status;
	memmove(pureg, uva, n);
	xp->status = status;
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg *xp, Proc *p)
{
	xp->pc = p->sched.pc;
	xp->sp = p->sched.sp;
	xp->r31 = (ulong)sched;
}
