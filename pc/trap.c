#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"errno.h"

void	noted(Ureg*, ulong);
void	notify(Ureg*);

/*
 *  8259 interrupt controllers
 */
enum
{
	Int0ctl=	0x20,		/* control port (ICW1, OCW2, OCW3) */
	Int0aux=	0x21,		/* everything else (ICW2, ICW3, ICW4, OCW1) */
	Int1ctl=	0xA0,		/* control port */
	Int1aux=	0xA1,		/* everything else (ICW2, ICW3, ICW4, OCW1) */

	EOI=		0x20,		/* non-specific end of interrupt */
};

int	int0mask = 7;		/* interrupts enabled for first 8259 */
int	int1mask = 7;		/* interrupts enabled for second 8259 */

/*
 *  trap/interrupt gates
 */
Segdesc ilt[256];
void	(*ivec[256])(void*);

void
sethvec(int v, void (*r)(void), int type, int pri)
{
	ilt[v].d0 = ((ulong)r)&0xFFFF|(KESEL<<16);
	ilt[v].d1 = ((ulong)r)&0xFFFF0000|SEGP|SEGPL(pri)|type;
}

void
setvec(int v, void (*r)(Ureg*))
{
	ivec[v] = r;

	/*
	 *  enable corresponding interrupt in 8259
	 */
	if((v&~0x7) == Int0vec){
		int0mask &= ~(1<<(v&7));
		outb(Int0aux, int0mask);
	}
}

/*
 *  set up the interrupt/trap gates
 */
void
trapinit(void)
{
	int i;

	/*
	 *  set the standard traps
	 */
	sethvec(0, intr0, SEGIG, 0);
	sethvec(1, intr1, SEGIG, 0);
	sethvec(2, intr2, SEGIG, 0);
	sethvec(3, intr3, SEGIG, 0);
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
	sethvec(14, intr14, SEGIG, 0);
	sethvec(15, intr15, SEGIG, 0);
	sethvec(16, intr16, SEGIG, 0);
	sethvec(17, intr17, SEGIG, 0);
	sethvec(18, intr18, SEGIG, 0);
	sethvec(19, intr19, SEGIG, 0);
	sethvec(20, intr20, SEGIG, 0);
	sethvec(21, intr21, SEGIG, 0);
	sethvec(22, intr22, SEGIG, 0);
	sethvec(23, intr23, SEGIG, 0);

	/*
	 *  set all others to unknown interrupts
	 */
	for(i = 24; i < 256; i++)
		sethvec(i, intrbad, SEGIG, 0);

	/*
	 *  system calls
	 */
	sethvec(64, intr64, SEGTG, 3);
	setvec(64, (void (*)(Ureg*))syscall);

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
	outb(Int0ctl, 0x11);		/* ICW1 - edge triggered, master,
					   ICW4 will be sent */
	outb(Int0aux, Int0vec);		/* ICW2 - interrupt vector offset */
	outb(Int0aux, 0x04);		/* ICW3 - master level 2 */
	outb(Int0aux, 0x01);		/* ICW4 - 8086 mode, not buffered */
}

/*
 *  All traps
 */
void
trap(Ureg *ur)
{
	if(ur->trap>=256 || ivec[ur->trap] == 0)
		panic("bad trap type %d %lux\n", ur->trap, ur->pc);

	/*
	 *  call the trap routine
	 */
	(*ivec[ur->trap])(ur);

	/*
	 *  tell the 8259 that we're done with the
	 *  highest level interrupt
	 */
	outb(Int0ctl, EOI);
}

/*
 *  dump registers
 */
void
dumpregs(Ureg *ur)
{
	if(u)
		print("registers for %s %d\n", u->p->text, u->p->pid);
	else
		print("registers for kernel\n");
	print("FLAGS=%lux ECODE=%lux CS=%lux PC=%lux SS=%lux USP=%lux\n", ur->flags,
		ur->ecode, ur->cs, ur->pc, ur->ss, ur->usp);

	print("  AX %8.8lux  BX %8.8lux  CX %8.8lux  DX %8.8lux\n",
		ur->ax, ur->bx, ur->cx, ur->dx);
	print("  SI %8.8lux  DI %8.8lux  BP %8.8lux  DS %8.8lux\n",
		ur->si, ur->di, ur->bp, ur->ds);
}

void
dumpstack(void)
{
}

void
execpc(ulong entry)
{
	((Ureg*)UREGADDR)->pc = entry;
}

/*
 *  system calls
 */
#undef	CHDIR	/* BUG */
#include "/sys/src/libc/9syscall/sys.h"

typedef long Syscall(ulong*);
Syscall	sysr1, sysfork, sysexec, sysgetpid, syssleep, sysexits, sysdeath, syswait;
Syscall	sysopen, sysclose, sysread, syswrite, sysseek, syserrstr, sysaccess, sysstat, sysfstat;
Syscall sysdup, syschdir, sysforkpgrp, sysbind, sysmount, syspipe, syscreate;
Syscall	sysbrk_, sysremove, syswstat, sysfwstat, sysnotify, sysnoted, sysalarm;

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
syscall(Ureg *ur)
{
	ulong	ax;
	ulong	sp;
	long	ret;
	int	i;

	u->p->insyscall = 1;
	u->p->pc = ur->pc;
	if((ur->cs)&0xffff == KESEL)
		panic("recursive system call");
	u->p->insyscall = 0;

	/*
	 *  do something about floating point!!!
	 */

	ax = ur->ax;
	sp = ur->usp;
	u->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(ax >= sizeof systab/BY2WD){
			pprint("bad sys call number %d pc %lux\n", ax, ur->pc);
			postnote(u->p, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}
		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-(1+MAXSYSARG)*BY2WD))
			validaddr(sp, (1+MAXSYSARG)*BY2WD, 0);
		ret = (*systab[ax])((ulong*)(sp+BY2WD));
		poperror();
	}
	if(u->nerrlab){
		print("bad errstack [%d]: %d extra\n", ax, u->nerrlab);
		for(i = 0; i < NERR; i++)
			print("sp=%lux pc=%lux\n", u->errlab[i].sp, u->errlab[i].pc);
		panic("error stack");
	}
	u->p->insyscall = 0;
	if(ax == NOTED){
		noted(ur, *(ulong*)(sp+BY2WD));
		ret = -1;
	} else if(u->nnote){
		ur->ax = ret;
		notify(ur);
	}
	return ret;
}

/*
 *  Call user, if necessary, with note.
 *  Pass user the Ureg struct and the note on his stack.
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
		u->svcs = ur->cs;
		u->svss = ur->ss;
		u->svflags = ur->flags;
		sp = ur->usp;
		sp -= sizeof(Ureg);
		if(waserror()){
			pprint("suicide: trap in notify\n");
			unlock(&u->p->debug);
			pexit("Suicide", 0);
		}
		validaddr((ulong)u->notify, 1, 0);
		validaddr(sp-ERRLEN-3*BY2WD, sizeof(Ureg)+ERRLEN-3*BY2WD, 0);
		poperror();
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
 *   Return user to state before notify()
 */
void
noted(Ureg *ur, ulong arg0)
{
	Ureg *nur;

	nur = u->ureg;
	validaddr(nur->pc, 1, 0);
	validaddr(nur->usp, BY2WD, 0);
	if(nur->cs!=u->svcs || nur->ss!=u->svss
	|| (nur->flags&0xff)!=(u->svflags&0xff)){
		pprint("bad noted ureg cs %ux ss %ux flags %ux\n", nur->cs, nur->ss,
			nur->flags);
		pexit("Suicide", 0);
	}
	lock(&u->p->debug);
	if(!u->notified){
		unlock(&u->p->debug);
		return;
	}
	u->notified = 0;
	nur->flags = (u->svflags&0xffffff00) | (ur->flags&0xff);
	memmove(ur, u->ureg, sizeof(Ureg));
	ur->ax = -1;	/* return error from the interrupted call */
	switch(arg0){
	case NCONT:
		splhi();
		unlock(&u->p->debug);
		return;

	default:
		pprint("unknown noted arg 0x%lux\n", arg0);
		u->lastnote.flag = NDebug;
		/* fall through */
		
	case NDFLT:
		if(u->lastnote.flag == NDebug)
			pprint("suicide: %s\n", u->lastnote.msg);
		unlock(&u->p->debug);
		pexit(u->lastnote.msg, u->lastnote.flag!=NDebug);
	}
}
