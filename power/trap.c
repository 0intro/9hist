#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"errno.h"

/*
 *  vme interrupt routines
 */
void	(*vmevec[256])(int);

void	noted(Ureg**);
void	rfnote(Ureg**);

#define	LSYS	0x01
#define	LUSER	0x02
/*
 * CAUSE register
 */

#define	EXCCODE(c)	((c>>2)&0x0F)
#define	FPEXC		16

char *excname[] =
{
	"external interrupt",
	"TLB modification",
	"TLB miss (load or fetch)",
	"TLB miss (store)",
	"address error (load or fetch)",
	"address error (store)",
	"bus error (fetch)",
	"bus error (data load or store)",
	"system call",
	"breakpoint",
	"reserved instruction",
	"coprocessor unusable",
	"arithmetic overflow",
	"undefined 13",
	"undefined 14",
	"undefined 15",				/* used as sys call for debugger */
	/* the following is made up */
	"floating point exception"		/* FPEXC */
};
char	*fpexcname(ulong);

char *regname[]={
	"STATUS",	"PC",
	"SP",		"CAUSE",
	"BADADDR",	"TLBVIRT",
	"HI",		"LO",
	"R31",		"R30",
	"R28",		"R27",
	"R26",		"R25",
	"R24",		"R23",
	"R22",		"R21",
	"R20",		"R19",
	"R18",		"R17",
	"R16",		"R15",
	"R14",		"R13",
	"R12",		"R11",
	"R10",		"R9",
	"R8",		"R7",
	"R6",		"R5",
	"R4",		"R3",
	"R2",		"R1",
};

long	ticks;

void
trap(Ureg *ur)
{
	int ecode;
	int user;
	ulong x;
	char buf[ERRLEN];

	SET(x);
	ecode = EXCCODE(ur->cause);
	user = ur->status&KUP;
	if(u)
		u->p->pc = ur->pc;		/* BUG */
	switch(ecode){
	case CINT:
		if(u && u->p->state==Running){
			if(u->p->fpstate == FPactive) {
				if(ur->cause & INTR3){	/* FP trap */
					x = clrfpintr();
					ecode = FPEXC;
				}
				savefpregs(&u->fpsave);
				u->p->fpstate = FPinactive;
				ur->status &= ~CU1;
				if(ecode == FPEXC)
					goto Default;
			}
		}
		intr(ur);
		break;

	case CTLBM:
	case CTLBL:
	case CTLBS:
		if(u == 0)
			panic("fault u==0 pc %lux addr %lux", ur->pc, ur->badvaddr);
		if(u->p->fpstate == FPactive) {
			savefpregs(&u->fpsave);
			u->p->fpstate = FPinactive;
			ur->status &= ~CU1;
		}
		spllo();
		x = u->p->insyscall;
		u->p->insyscall = 1;
		faultmips(ur, user, ecode);
		u->p->insyscall = x;
		break;

	case CCPU:
		if(u && u->p && u->p->fpstate == FPinit) {
			restfpregs(&initfp, u->fpsave.fpstatus);
			u->p->fpstate = FPactive;
			ur->status |= CU1;
			break;
		}
		if(u && u->p && u->p->fpstate == FPinactive) {
			restfpregs(&u->fpsave, u->fpsave.fpstatus);
			u->p->fpstate = FPactive;
			ur->status |= CU1;
			break;
		}
		goto Default;

	default:
		if(u && u->p && u->p->fpstate == FPactive){
			savefpregs(&u->fpsave);
			u->p->fpstate = FPinactive;
			ur->status &= ~CU1;
		}
	Default:
		/*
		 * This isn't good enough; can still deadlock because we may hold print's locks
		 * in this processor.
		 */
		if(user){
			spllo();
			if(ecode == FPEXC)
				sprint(buf, "sys: fp: %s FCR31 %lux", fpexcname(x), x);
			else
				sprint(buf, "sys: trap: %s[%d]", excname[ecode], m->machno);

			postnote(u->p, 1, buf, NDebug);
		}else{
			print("%s %s pc=%lux\n", user? "user": "kernel", excname[ecode], ur->pc);
			if(ecode == FPEXC)
				print("fp: %s FCR31 %lux\n", fpexcname(x), x);
			dumpregs(ur);
			if(m->machno == 0)
				spllo();
			exit();
		}
	}
	if(user && u->nnote)
		notify(ur);
	splhi();
	if(user && u && u->p->fpstate == FPinactive) {
		restfpregs(&u->fpsave, u->fpsave.fpstatus);
		u->p->fpstate = FPactive;
		ur->status |= CU1;
	}
}

void
intr(Ureg *ur)
{
	int i, any;
	uchar xxx;
	uchar pend, npend;
	long v;
	ulong cause;
	static int bogies;

	m->intr++;
	cause = ur->cause&(INTR5|INTR4|INTR3|INTR2|INTR1);
	if(cause & (INTR2|INTR4)){
		clock(ur);
		cause &= ~(INTR2|INTR4);
	}
	if(cause & INTR1){
		duartintr();
		cause &= ~INTR1;
	}
	if(cause & INTR5){
		any = 0;
		if(!(*MPBERR1 & (1<<8))){
			print("MP bus error %lux %lux\n", *MPBERR0, *MPBERR1);
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
		npend = pend;

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
			npend = pend |= xxx;
			i = SBCCREG->flevel;
			USED(i);
		}

		/*
		 *  5a. process lance, scsi
		 */
		if(pend & 1) {
			v = INTVECREG->i[0].vec;
			if(!(v & (1<<12))){
				print("io2 mp bus error %d %lux %lux\n", 0,
					*MPBERR0, *MPBERR1);
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
				if(v & (1<<1))
					print("SCSI 1 interrupt\n");
				if(v & (1<<0))
					print("SCSI 0 interrupt\n");
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
		 *  if nothing else, assume bus error
		 */
		if(!any && bogies++<100){
			print("bogus intr lvl 5 pend %lux on %d\n", npend, m->machno);
			delay(100);
		}
		/*
		 *  6. re-enable interrupts
		 */
		*IO2SETMASK = 0xff000000;
		cause &= ~INTR5;
	}
	if(cause)
		panic("cause %lux %lux\n", u, cause);
}

char *
fpexcname(ulong x)
{
	static char *str[]={
		"inexact operation",
		"underflow",
		"overflow",
		"division by zero",
		"invalid operation",
		"unimplemented operation",
	};
	int i;

	x >>= 12;
	for(i=0; i<6; i++, x>>=1)
		if(x & 1)
			return str[i];
	return "no floating point exception";
}

void
dumpstack(void)
{
	ulong l, v;
	extern ulong etext;

	if(u)
		for(l=(ulong)&l; l<USERADDR+BY2PG; l+=4){
			v = *(ulong*)l;
			if(KTZERO < v && v < (ulong)&etext)
				print("%lux=%lux\n", l, v);
		}
}

void
dumpregs(Ureg *ur)
{
	int i;
	ulong *l;
	int (*pr)(char*, ...);

	if(u)
		print("registers for %s %d\n", u->p->text, u->p->pid);
	else
		print("registers for kernel\n");
	l = &ur->status;
	for(i=0; i<sizeof regname/sizeof(char*); i+=2, l+=2)
		print("%s\t%.8lux\t%s\t%.8lux\n", regname[i], l[0], regname[i+1], l[1]);
}

/*
 * Call user, if necessary, with note
 */
void
notify(Ureg *ur)
{
	ulong sp;

	lock(&u->p->debug);
	if(u->nnote==0){
		unlock(&u->p->debug);
		return;
	}
	if(u->note[0].flag!=NUser && (u->notified || u->notify==0)){
		if(u->note[0].flag == NDebug)
			pprint("suicide: %s\n", u->note[0].msg);
    Die:
		unlock(&u->p->debug);
		pexit(u->note[0].msg, u->note[0].flag!=NDebug);
	}
	if(!u->notified){
		if(!u->notify)
			goto Die;
		u->svstatus = ur->status;
		sp = ur->usp;
		sp -= sizeof(Ureg);
		u->ureg = (void*)sp;
		memmove((Ureg*)sp, ur, sizeof(Ureg));
		sp -= ERRLEN;
		memmove((char*)sp, u->note[0].msg, ERRLEN);
		sp -= 3*BY2WD;
		*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
		*(ulong*)(sp+1*BY2WD) = (ulong)u->ureg;	/* arg 1 is ureg* */
		*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
		ur->usp = sp;
		ur->pc = (ulong)u->notify;
		u->notified = 1;
		u->nnote--;
		memmove(&u->note[0], &u->note[1], u->nnote*sizeof(Note));
	}
	unlock(&u->p->debug);
}

/*
 * Return user to state before notify()
 */
void
noted(Ureg **urp)
{
	Ureg *nur;

	nur = u->ureg;
	if(waserror()){
		pprint("suicide: trap in noted\n");
		pexit("Suicide", 0);
	}
	validaddr(nur->pc, 1, 0);
	validaddr(nur->usp, BY2WD, 0);
	poperror();
	if(nur->status!=u->svstatus){
		pprint("bad noted ureg status %ux\n", nur->status);
		pexit("Suicide", 0);
	}
	lock(&u->p->debug);
	if(!u->notified){
		unlock(&u->p->debug);
		return;
	}
	u->notified = 0;
	memmove(*urp, u->ureg, sizeof(Ureg));
	(*urp)->r1 = -1;	/* return error from the interrupted call */
	unlock(&u->p->debug);
	splhi();
	rfnote(urp);
}


#undef	CHDIR	/* BUG */
#include "/sys/src/libc/mips9sys/sys.h"

typedef long Syscall(ulong*);
Syscall sysbind, sysbrk_, syschdir, sysclose, syscreate, sysdeath;
Syscall	sysdup, syserrstr, sysexec, sysexits, sysfork, sysforkpgrp;
Syscall	sysfstat, sysfwstat, sysgetpid, sysmount, sysnoted;
Syscall	sysnotify, sysopen, syspipe, sysr1, sysread, sysremove, sysseek;
Syscall syssleep, sysstat, syswait, syswrite, syswstat, sysalarm;

Syscall *systab[]={
	[SYSR1]		sysr1,
	[ERRSTR]	syserrstr,
	[BIND]		sysbind,
	[CHDIR]		syschdir,
	[CLOSE]		sysclose,
	[DUP]		sysdup,
	[ALARM]		sysalarm,
	[EXEC]		sysexec,
	[EXITS]		sysexits,
	[FORK]		sysfork,
	[FORKPGRP]	sysforkpgrp,
	[FSTAT]		sysfstat,
	[___LASTERR___]	sysdeath,
	[MOUNT]		sysmount,
	[OPEN]		sysopen,
	[READ]		sysread,
	[SEEK]		sysseek,
	[SLEEP]		syssleep,
	[STAT]		sysstat,
	[WAIT]		syswait,
	[WRITE]		syswrite,
	[PIPE]		syspipe,
	[CREATE]	syscreate,
	[___USERSTR___]	sysdeath,
	[BRK_]		sysbrk_,
	[REMOVE]	sysremove,
	[WSTAT]		syswstat,
	[FWSTAT]	sysfwstat,
	[NOTIFY]	sysnotify,
	[NOTED]		sysnoted,
};

long
syscall(Ureg *aur)
{
	long ret;
	ulong sp;
	ulong r1;
	Ureg *ur;
	char *msg;

	m->syscall++;
	u->p->insyscall = 1;
	ur = aur;
	u->p->pc = ur->pc;		/* BUG */
	ur->cause = 15<<2;		/* for debugging: system call is undef 15;
	/*
	 * since the system call interface does not
	 * guarantee anything about registers, we can
	 * smash them.  but we must save fpstatus.
	 */
	if(u->p->fpstate == FPactive) {
		u->fpsave.fpstatus = fcr31();
		u->p->fpstate = FPinit;
		ur->status &= ~CU1;
	}
	spllo();
	r1 = ur->r1;
	sp = ur->sp;
	u->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(r1 >= sizeof systab/BY2WD){
			pprint("bad sys call number %d pc %lux\n", r1, ((Ureg*)UREGADDR)->pc);
			msg = "sys: bad sys call";
	    Bad:
			postnote(u->p, 1, msg, NDebug);
			error(Ebadarg);
		}
		if(sp & (BY2WD-1)){
			pprint("odd sp in sys call pc %lux sp %lux\n", ((Ureg*)UREGADDR)->pc, ((Ureg*)UREGADDR)->sp);
			msg = "sys: odd stack";
			goto Bad;
		}
		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-5*BY2WD))
			validaddr(sp, 5*BY2WD, 0);
		ret = (*systab[r1])((ulong*)(sp+2*BY2WD));
	}
	ur->pc += 4;
	u->nerrlab = 0;
	splhi();
	u->p->insyscall = 0;
	if(r1 == NOTED)	/* ugly hack */
		noted(&aur);	/* doesn't return */
	if(u->nnote){
		ur->r1 = ret;
		notify(ur);
	}
	return ret;
}

void
execpc(ulong entry)
{
	((Ureg*)UREGADDR)->pc = entry - 4;		/* syscall advances it */
}

#include "errstr.h"

void
error(int code)
{
	strncpy(u->error, errstrtab[code], ERRLEN);
	nexterror();
}

void
errors(char *err)
{
	strncpy(u->error, err, ERRLEN);
	nexterror();
}

void
nexterror(void)
{
	gotolabel(&u->errlab[--u->nerrlab]);
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
	if(g && g != novme)
		print("second setvmevec to 0x%.2x\n", v);
	vmevec[v] = f;
}
