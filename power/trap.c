#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"

/*
 *  vme interrupt routines
 */
void	(*vmevec[256])(int);

void	notify(Ureg*);
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
	"undefined 15",
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

	ecode = EXCCODE(ur->cause);
	user = ur->status&KUP;
	if(u)
		u->p->pc = ur->pc;		/* BUG */
	switch(ecode){
	case CINT:
		m->intrp = 0;
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
			m->intr = intr;
			m->cause = ur->cause;
			if(ur->cause & INTR2)
				m->intrp = u->p;
			sched();
		}else
			intr(ur->cause);
		break;

	case CTLBM:
	case CTLBL:
	case CTLBS:
		if(u == 0)
			panic("fault");
		if(u->p->fpstate == FPactive) {
			savefpregs(&u->fpsave);
			u->p->fpstate = FPinactive;
			ur->status &= ~CU1;
		}
		spllo();
		x = u->p->insyscall;
		u->p->insyscall = 1;
		fault(ur, user, ecode);
		u->p->insyscall = x;
		break;

	case CCPU:
		if(u->p->fpstate == FPinit) {
			restfpregs(&initfp);
			u->p->fpstate = FPactive;
			ur->status |= CU1;
			break;
		}
		if(u->p->fpstate == FPinactive) {
			restfpregs(&u->fpsave);
			u->p->fpstate = FPactive;
			ur->status |= CU1;
			break;
		}

	default:
	Default:
		/*
		 * This isn't good enough; can still deadlock because we may hold print's locks
		 * in this processor.
		 */
		if(user){
			spllo();
			if(ecode == FPEXC)
				sprint(buf, "fp: %s FCR31 %lux", fpexcname(x), x);
			else
				sprint(buf, "trap: %s", excname[ecode]);
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
		restfpregs(&u->fpsave);
		u->p->fpstate = FPactive;
		ur->status |= CU1;
	}
}

void
intr(ulong cause)
{
	int i, pend;
	long v;

	cause &= INTR5|INTR4|INTR3|INTR2|INTR1;
	if(cause & (INTR2|INTR4)){
		clock(cause);
		cause &= ~(INTR2|INTR4);
	}
	if(cause & INTR1){
		duartintr();
		cause &= ~INTR1;
	}
	if(cause & INTR5){

		if(!(*MPBERR1 & (1<<8))){
/*			print("MP bus error %lux\n", *MPBERR0); /**/
			*MPBERR0 = 0;
			i = *SBEADDR;
		}

		/*
		 *  directions from IO2 manual
		 *  1. clear all IO2 masks
		 */
		*IO2CLRMASK = 0xff;

		/*
		 *  2. wait for interrupt in progress
		 */
		while(!(*INTPENDREG & (1<<5)))
			;

		/*
		 *  3. read pending interrupts
		 */
		pend = SBCCREG->fintpending & 0xff;

		/*
		 *  4. clear pending register
		 */
		i = SBCCREG->flevel;

		/*
		 *  5a. process lance, scsi
		 */
	loop:
		if(pend & 1) {
			v = INTVECREG->i[0].vec;
/* a botch, bit 12 seems to always be on
			if(v & (1<<12))
				print("io2 mp bus error %d\n", 0);
 */
			if(!(v & (1<<2)))
				lanceintr();
			if(!(v & (1<<1)))
				lanceparity();
			if(!(v & (1<<0)))
				print("SCSI interrupt\n");
		}
		/*
		 *  5b. process vme
		 *  i bet i can guess your level
		 */
		pend >>= 1;
		for(i=1; pend; i++) {
			if(pend & 1) {
				v = INTVECREG->i[i].vec;
/* a botch, bit 12 seems to always be on
				if(v & (1<<12))
					print("io2 mp bus error %d\n", i);
 */
				v &= 0xff;
				(*vmevec[v])(v);
			}
			pend >>= 1;
		}
		/*
		 *  6. re-enable interrupts
		 */
		*IO2SETMASK = 0xff;
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
	dumpstack();
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
		sp = ur->sp;
		sp -= sizeof(Ureg);
		u->ureg = (void*)sp;
		memcpy((Ureg*)sp, ur, sizeof(Ureg));
		sp -= ERRLEN;
		memcpy((char*)sp, u->note[0].msg, ERRLEN);
		sp -= 3*BY2WD;
		*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
		*(ulong*)(sp+1*BY2WD) = (ulong)u->ureg;	/* arg 1 is ureg* */
		*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
		ur->sp = sp;
		ur->pc = (ulong)u->notify;
		u->notified = 1;
		u->nnote--;
		memcpy(&u->note[0], &u->note[1], u->nnote*sizeof(Note));
	}
	unlock(&u->p->debug);
}

/*
 * Return user to state before notify()
 */
void
noted(Ureg **urp)
{
	lock(&u->p->debug);
	u->notified = 0;
	memcpy(*urp, u->ureg, sizeof(Ureg));
	unlock(&u->p->debug);
	splhi();
	rfnote(urp);
}


#undef	CHDIR	/* BUG */
#include "/sys/src/libc/mips9sys/sys.h"

typedef long Syscall(ulong*);
Syscall sysaccess, sysbind, sysbrk_, syschdir, sysclose, syscreate;
Syscall	sysdup, syserrstr, sysexec, sysexits, sysfork, sysforkpgrp;
Syscall	sysfstat, sysfwstat, sysgetpid, syslasterr, sysmount, sysnoted;
Syscall	sysnotify, sysopen, syspipe, sysr1, sysread, sysremove, sysseek;
Syscall syssleep, sysstat, sysuserstr, syswait, syswrite, syswstat;

Syscall *systab[]={
	[SYSR1]		sysr1,
	[ACCESS]	sysaccess,
	[BIND]		sysbind,
	[CHDIR]		syschdir,
	[CLOSE]		sysclose,
	[DUP]		sysdup,
	[ERRSTR]	syserrstr,
	[EXEC]		sysexec,
	[EXITS]		sysexits,
	[FORK]		sysfork,
	[FORKPGRP]	sysforkpgrp,
	[FSTAT]		sysfstat,
	[LASTERR]	syslasterr,
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
	[USERSTR]	sysuserstr,
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

	u->p->insyscall = 1;
	ur = aur;
	/*
	 * since the system call interface does not
	 * guarantee anything about registers,
	 */
	if(u->p->fpstate == FPactive) {
		u->p->fpstate = FPinit;		/* BUG */
		ur->status &= ~CU1;
	}
	spllo();
	r1 = ur->r1;
	sp = ur->sp;
	if(r1 >= sizeof systab/BY2WD)
		panic("syscall %d\n", r1);
	if(sp & (BY2WD-1))
		panic("syscall odd sp");
	if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-4*BY2WD))
		validaddr(ur->sp, 4*BY2WD, 0);

	u->nerrlab = 0;
	ret = -1;
	if(!waserror())
		ret = (*systab[r1])((ulong*)(sp+2*BY2WD));
	ur->pc += 4;
	u->nerrlab = 0;
	splhi();
	if(r1 == NOTED)	/* ugly hack */
		noted(&aur);	/* doesn't return */
	if(u->nnote){
		ur->r1 = ret;
		notify(ur);
	}
	u->p->insyscall = 0;
	return ret;
}

void
error(Chan *c, int code)
{
	if(c){
		u->error.type = c->type;
		u->error.dev = c->dev;
	}else{
		u->error.type = 0;
		u->error.dev = 0;
	}
	u->error.code = code;
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
