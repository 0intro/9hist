#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"errno.h"

void	notify(Ureg*);
void	noted(Ureg**);
void	rfnote(Ureg**);

extern	void traplink(void);
extern	void syslink(void);

long	ticks;

char *trapname[]={
	"reset",
	"instruction access exception",
	"illegal instruction",
	"privileged instruction",
	"fp disabled",
	"window overflow",
	"window underflow",
	"unaligned address",
	"fp exception",
	"data access exception",
	"tag overflow",
};

char*
excname(ulong tbr)
{
	static char buf[32];	/* BUG: not reentrant! */

	if(tbr < sizeof trapname/sizeof(char*))
		return trapname[tbr];
	if(tbr == 36)
		return "cp disabled";
	if(tbr == 40)
		return "cp exception";
	if(tbr >= 128)
		sprint(buf, "trap instruction %d", tbr-128);
	else if(17<=tbr && tbr<=31)
		sprint(buf, "interrupt level %d", tbr-16);
	else
		sprint(buf, "unknown trap %d", tbr);
	return buf;
}

void
trap(Ureg *ur)
{
	int user, x;
	char buf[64];
	ulong tbr;

	if(u)
		u->p->pc = ur->pc;		/* BUG */
	user = !(ur->psr&PSRPSUPER);
	tbr = (ur->tbr&0xFFF)>>4;
	if(tbr > 16){			/* interrupt */
		if(u && u->p->state==Running){
			if(u->p->fpstate == FPactive) {
				savefpregs(&u->fpsave);
				u->p->fpstate = FPinactive;
				ur->psr &= ~PSREF;
			}
		}
		switch(tbr-16){
		case 15:			/* asynch mem err */
			faultasync(ur);
			break;
		case 14:			/* counter 1 */
			clock(ur);
			break;
		case 12:			/* keyboard and mouse */
			duartintr();
			break;
		case 5:				/* lance */
			lanceintr();
			break;
		default:
			goto Error;
		}
	}else{
		switch(tbr){
		case 1:				/* instr. access */
		case 9:				/* data access */
			if(u && u->p->fpstate==FPactive) {
				savefpregs(&u->fpsave);
				u->p->fpstate = FPinactive;
				ur->psr &= ~PSREF;
			}
			if(u){
				x = u->p->insyscall;
				u->p->insyscall = 1;
				faultsparc(ur);
				u->p->insyscall = x;
			}else
				faultsparc(ur);
			goto Return;
		case 4:				/* floating point disabled */
			if(u && u->p){
				if(u->p->fpstate == FPinit)
					restfpregs(&initfp);
				else if(u->p->fpstate == FPinactive)
					restfpregs(&u->fpsave);
				else
					break;
				u->p->fpstate = FPactive;
				ur->psr |= PSREF;
				return;
			}
			break;
		case 8:				/* floating point exception */
			clearfpintr();
			break;
		default:
			break;
		}
		if(user){
			spllo();
			sprint(buf, "sys: trap: pc=0x%lux %s", ur->pc, excname(tbr));
			if(tbr == 8)
				sprint(buf+strlen(buf), " FSR %lux", u->fpsave.fsr);
			postnote(u->p, 1, buf, NDebug);
		}else{
    Error:
			print("kernel trap: %s pc=0x%lux\n", excname(tbr), ur->pc);
			dumpregs(ur);
			for(;;);
		}
		if(user && u->nnote)
			notify(ur);
	}
    Return:
	if(user && u && u->p->fpstate == FPinactive) {
		restfpregs(&u->fpsave);
		u->p->fpstate = FPactive;
		ur->psr |= PSREF;
	}
}

void
trapinit(void)
{
	int i;
	long t, a;

	a = ((ulong)traplink-TRAPS)>>2;
	a += 0x40000000;			/* CALL traplink(SB) */
	t = TRAPS;
	for(i=0; i<256; i++){
		*(ulong*)t = a;			/* CALL traplink(SB) */
		*(ulong*)(t+4) = 0xa7480000;	/* MOVW PSR, R19 */
		a -= 16/4;
		t += 16;
	}
	/*
	 * Vector 128 goes directly to syslink
	 */
	t = TRAPS+128*16;
	a = ((ulong)syslink-t)>>2;
	a += 0x40000000;
	*(ulong*)t = a;			/* CALL syscall(SB) */
	*(ulong*)(t+4) = 0xa7480000;	/* MOVW PSR, R19 */
	puttbr(TRAPS);
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
	print("PSR=%ux PC=%lux TBR=%lux\n", ur->psr, ur->pc, ur->tbr);
	l = &ur->r0;
	for(i=0; i<32; i+=2, l+=2)
		print("R%d\t%.8lux\tR%d\t%.8lux\n", i, l[0], i+1, l[1]);
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
		ur->npc = (ulong)u->notify+4;
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
	lock(&u->p->debug);
	if(!u->notified){
		unlock(&u->p->debug);
		return;
	}
	u->notified = 0;
	memmove(*urp, u->ureg, sizeof(Ureg));
	(*urp)->r7 = -1;	/* return error from the interrupted call */
	unlock(&u->p->debug);
	splhi();
	rfnote(urp);
}

#undef	CHDIR	/* BUG */
#include "/sys/src/libc/sparc9sys/sys.h"

typedef long Syscall(ulong*);
Syscall	sysr1, sysfork, sysexec, sysgetpid, syssleep, sysexits, sysdeath, syswait;
Syscall	sysopen, sysclose, sysread, syswrite, sysseek, syserrstr, sysaccess, sysstat, sysfstat;
Syscall sysdup, syschdir, sysforkpgrp, sysbind, sysmount, syspipe, syscreate;
Syscall	sysbrk_, sysremove, syswstat, sysfwstat, sysnotify, sysnoted;

Syscall *systab[]={
	sysr1,
	syserrstr,
	sysbind,
	syschdir,
	sysclose,
	sysdup,
	sysdeath,
	sysexec,
	sysexits,
	sysfork,
	sysforkpgrp,
	sysfstat,
	sysdeath,
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
	sysdeath,	/* sysfilsys */
};

long
syscall(Ureg *aur)
{
	long ret;
	ulong sp;
	ulong r7;
	Ureg *ur;
	char *msg;

	u->p->insyscall = 1;
	ur = aur;
	u->p->pc = ur->pc;
	if(ur->psr & PSRPSUPER)
		panic("recursive system call");

	/*
	 * since the system call interface does not
	 * guarantee anything about registers,
	 */
	if(u->p->fpstate == FPactive) {
		u->p->fpstate = FPinit;
		ur->psr &= ~PSREF;
	}

	spllo();
	r7 = ur->r7;
	sp = ur->usp;

	u->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(r7 >= sizeof systab/BY2WD){
			pprint("bad sys call number %d pc %lux\n", r7, ((Ureg*)UREGADDR)->pc);
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
		ret = (*systab[r7])((ulong*)(sp+2*BY2WD));
	}
	ur->pc += 4;
	ur->npc = ur->pc+4;
	u->nerrlab = 0;
	u->p->insyscall = 0;
	if(r7 == NOTED)	/* ugly hack */
		noted(&aur);	/* doesn't return */
	if(u->nnote){
		ur->r7 = ret;
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
	strncpy(u->error, err, NAMELEN);
	nexterror();
}


void
nexterror(void)
{
	gotolabel(&u->errlab[--u->nerrlab]);
}
