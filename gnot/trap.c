#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"errno.h"

void	notify(Ureg*);
void	noted(Ureg*, ulong);
void	rfnote(Ureg*);

char *regname[]={
	"R0",
	"R1",
	"R2",
	"R3",
	"R4",
	"R5",
	"R6",
	"R7",
	"A0",
	"A1",
	"A2",
	"A3",
	"A4",
	"A5",
	"A6",
	"A7",
};

long	ticks;

char *trapname[]={
	"reset isp",
	"reset ipc",
	"bus error",
	"address error",
	"illegal instruction",
	"zero divide",
	"chk, chk2 instruction",
	"cptrapcc, trapcc, trapv instruction",
	"privilege violation",
	"trace",
	"line 1010 emulator",
	"line 1111 emulator",
	"reserved",
	"coprocessor protocol violation",
	"format error",
	"uninitialized interrupt",
	"unassigned 0x40",
	"unassigned 0x44",
	"unassigned 0x48",
	"unassigned 0x4C",
	"unassigned 0x50",
	"unassigned 0x54",
	"unassigned 0x58",
	"unassigned 0x5C",
	"spurious interrupt",
	"level 1 autovector (tac)",
	"level 2 autovector (port)",
	"level 3 autovector (incon)",
	"level 4 autovector (mouse)",
	"level 5 autovector (uart)",
	"level 6 autovector (sync)",
	"level 7 autovector",
};

char*
excname(unsigned vo)
{
	static char buf[32];	/* BUG: not reentrant! */

	vo &= 0x0FFF;
	vo >>= 2;
	if(vo < sizeof trapname/sizeof(char*))
		return trapname[vo];
	sprint(buf, "offset 0x%ux", vo<<2);
	return buf;
}

void
trap(Ureg *ur)
{
	int user;
	char buf[64];

	user = !(ur->sr&SUPER);

	if(u)
		u->p->pc = ur->pc;		/* BUG */
	if(user){
		sprint(buf, "sys: %s pc=0x%lux", excname(ur->vo), ur->pc);
		postnote(u->p, 1, buf, NDebug);
	}else{
		print("kernel trap vo=0x%ux pc=0x%lux\n", ur->vo, ur->pc);
		dumpregs(ur);
		exit();
	}

	if(user && u->nnote)
		notify(ur);
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

	if(u)
		print("registers for %s %d\n", u->p->text, u->p->pid);
	else
		print("registers for kernel\n");
	print("SR=%ux PC=%lux VO=%lux, USP=%lux\n", ur->sr, ur->pc, ur->vo, ur->usp);
	l = &ur->r0;
	for(i=0; i<sizeof regname/sizeof(char*); i+=2, l+=2)
		print("%s\t%.8lux\t%s\t%.8lux\n", regname[i], l[0], regname[i+1], l[1]);
	for(;;);
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
		u->svvo = ur->vo;
		u->svsr = ur->sr;
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
		memmove(&u->lastnote, &u->note[0], sizeof(Note));
		memmove(&u->note[0], &u->note[1], u->nnote*sizeof(Note));
	}
	unlock(&u->p->debug);
}

/*
 * Return user to state before notify()
 */
void
noted(Ureg *ur, ulong arg0)
{
	Ureg *nur;

	nur = u->ureg;
	validaddr(nur->pc, 1, 0);
	validaddr(nur->usp, BY2WD, 0);
	if(nur->sr!=u->svsr || nur->vo!=u->svvo){
		pprint("bad noted ureg sr %ux vo %ux\n", nur->sr, nur->vo);
		pexit("Suicide", 0);
	}
	lock(&u->p->debug);
	if(!u->notified){
		unlock(&u->p->debug);
		return;
	}
	u->notified = 0;
	memmove(ur, u->ureg, sizeof(Ureg));
	ur->r0 = -1;	/* return error from the interrupted call */
	switch(arg0){
	case NCONT:
		splhi();
		unlock(&u->p->debug);
		rfnote(ur);
		break;
		/* never returns */

	default:
		pprint("unknown noted arg 0x%lux\n", arg0);
		u->lastnote.flag = NDebug;
		/* fall through */
		
	case NTERM:
		if(u->lastnote.flag == NDebug)
			pprint("suicide: %s\n", u->lastnote.msg);
		unlock(&u->p->debug);
		pexit(u->lastnote.msg, u->lastnote.flag!=NDebug);
	}
}

#undef	CHDIR	/* BUG */
#include "/sys/src/libc/9syscall/sys.h"

typedef long Syscall(ulong*);
Syscall	sysr1, sysfork, sysexec, sysgetpid, syssleep, sysexits, sysdeath, syswait;
Syscall	sysopen, sysclose, sysread, syswrite, sysseek, syserrstr, sysaccess, sysstat, sysfstat;
Syscall sysdup, syschdir, sysforkpgrp, sysbind, sysmount, syspipe, syscreate;
Syscall	sysbrk_, sysremove, syswstat, sysfwstat, sysnotify, sysnoted, sysalarm;
Syscall syssegattach, syssegdetach, syssegfree, syssegflush, syssegbrk;

Syscall *systab[]={
	sysr1,
	syserrstr,
	sysbind,
	syschdir,
	sysclose,
	sysdup,
	sysalarm,
	sysexec,
	sysexits,
	sysfork,
	sysforkpgrp,
	sysfstat,
	syssegbrk,
	sysmount,
	sysopen,
	sysread,
	sysseek,
	syssleep,
	sysstat,
	syswait,
	syswrite,
	syspipe,
	syscreate,
	sysdeath,
	sysbrk_,
	sysremove,
	syswstat,
	sysfwstat,
	sysnotify,
	sysnoted,
	syssegattach,
	syssegdetach,
	syssegfree,
	syssegflush,
};

long
syscall(Ureg *aur)
{
	int i;
	long ret;
	ulong sp;
	ulong r0;
	Ureg *ur;
	char *msg;

	u->p->insyscall = 1;
	ur = aur;
	u->p->pc = ur->pc;
	if(ur->sr & SUPER)
		panic("recursive system call");
	/*
	 * since the system call interface does not
	 * guarantee anything about registers, but the fpcr is more than
	 * just a register...  BUG
	 */
	splhi();
	fpsave(&u->fpsave);
	if(u->p->fpstate==FPactive || u->fpsave.type){
		fprestore(&initfp);
		u->p->fpstate = FPinit;
		m->fpstate = FPinit;
	}
	spllo();
	r0 = ur->r0;
	sp = ur->usp;

	u->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(r0 >= sizeof systab/BY2WD){
			pprint("bad sys call number %d pc %lux\n", r0, ((Ureg*)UREGADDR)->pc);
			msg = "sys: bad sys call";
	    Bad:
			postnote(u->p, 1, msg, NDebug);
			error(Ebadarg);
		}
		if(sp & (BY2WD-1)){
			pprint("odd sp in sys call pc %lux sp %lux\n", 
				((Ureg*)UREGADDR)->pc, ((Ureg*)UREGADDR)->sp);
			msg = "sys: odd stack";
			goto Bad;
		}
		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-(1+MAXSYSARG)*BY2WD))
			validaddr(sp, (1+MAXSYSARG)*BY2WD, 0);
		ret = (*systab[r0])((ulong*)(sp+BY2WD));
		poperror();
	}

	if(u->nerrlab){
		print("bad errstack [%d]: %d extra\n", r0, u->nerrlab);
		for(i = 0; i < NERR; i++)
			print("sp=%lux pc=%lux\n", u->errlab[i].sp, u->errlab[i].pc);
		panic("error stack");
	}

	u->p->insyscall = 0;

	if(r0 == NOTED)		/* ugly hack */
		noted(aur, *(ulong*)(sp+BY2WD));	/* doesn't return */
	if(u->nnote){
		ur->r0 = ret;
		notify(ur);
	}
	return ret;
}

void
execpc(ulong entry)
{
	((Ureg*)UREGADDR)->pc = entry;
}
