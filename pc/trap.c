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

void	intr0(void), intr1(void), intr2(void), intr3(void);
void	intr4(void), intr5(void), intr6(void), intr7(void);
void	intr8(void), intr9(void), intr10(void), intr11(void);
void	intr12(void), intr13(void), intr14(void), intr15(void);
void	intr16(void);
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

	EOI=		0x20,		/* non-specific end of interrupt */
};

int	int0mask = 0x00;	/* interrupts enabled for first 8259 */
int	int1mask = 0x00;	/* interrupts enabled for second 8259 */

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
	} else if((v&~0x7) == Int1vec){
		int1mask &= ~(1<<(v&7));
		outb(Int1aux, int1mask);
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
	 *  set all interrupts to panics
	 */
	for(i = 0; i < 256; i++)
		sethvec(i, intrbad, SEGTG, 0);

	/*
	 *  80386 processor (and coprocessor) traps
	 */
	sethvec(0, intr0, SEGTG, 0);
	sethvec(1, intr1, SEGTG, 0);
	sethvec(2, intr2, SEGTG, 0);
	sethvec(3, intr3, SEGTG, 0);
	sethvec(4, intr4, SEGTG, 0);
	sethvec(5, intr5, SEGTG, 0);
	sethvec(6, intr6, SEGTG, 0);
	sethvec(7, intr7, SEGTG, 0);
	sethvec(8, intr8, SEGTG, 0);
	sethvec(9, intr9, SEGTG, 0);
	sethvec(10, intr10, SEGTG, 0);
	sethvec(11, intr11, SEGTG, 0);
	sethvec(12, intr12, SEGTG, 0);
	sethvec(13, intr13, SEGTG, 0);
	sethvec(14, intr14, SEGTG, 0);
	sethvec(15, intr15, SEGTG, 0);
	sethvec(16, intr16, SEGTG, 0);

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
	outb(Int0aux, 0x04);		/* ICW3 - have slave on level 2 */
	outb(Int0aux, 0x01);		/* ICW4 - 8086 mode, not buffered */

	/*
	 *  Set up the second 8259 interrupt processor.
	 *  Make 8259 interrupts start at CPU vector Int0vec.
	 *  Set the 8259 as master with edge triggered
	 *  input with fully nested interrupts.
	 */
	outb(Int1ctl, 0x11);		/* ICW1 - edge triggered, master,
					   ICW4 will be sent */
	outb(Int1aux, Int1vec);		/* ICW2 - interrupt vector offset */
	outb(Int1aux, 0x02);		/* ICW3 - I am a slave on level 2 */
	outb(Int1aux, 0x01);		/* ICW4 - 8086 mode, not buffered */

	/*
	 *  pass #2 8259 interrupts to #1
	 */
	int0mask &= ~0x04;
	outb(Int0aux, int0mask);
}

/*
 *  All traps
 */
void
trap(Ureg *ur)
{
	int v;
	int c;

	v = ur->trap;
	if(v>=256 || ivec[v] == 0){
		panic("bad trap type %d %lux %lux %lux\n", v, ur->pc, int0mask, int1mask);
		return;
	}

	/*
	 *  tell the 8259 that we're done with the
	 *  highest level interrupt (interrupts are still
	 *  off at this point)
	 */
	c = v&~0x7;
	if(c==Int0vec || c==Int1vec){
		if(c == Int1vec)
			outb(Int1ctl, EOI);
		outb(Int0ctl, EOI);
		if(v != Uart0vec)
			uartintr0(ur);
	}

	/*
	 *  call the trap routine
	 */
	(*ivec[v])(ur);
}

/*
 *  print 8259 status
 */
void
dump8259(void)
{
	int c;

	outb(Int0ctl, 0x0a);	/* read ir */
	print("ir0 %lux\n", inb(Int0ctl));
	outb(Int0ctl, 0x0b);	/* read is */
	print("is0 %lux\n", inb(Int0ctl));
	print("im0 %lux\n", inb(Int0aux));

	outb(Int1ctl, 0x0a);	/* read ir */
	print("ir1 %lux\n", inb(Int1ctl));
	outb(Int1ctl, 0x0b);	/* read is */
	print("is1 %lux\n", inb(Int1ctl));
	print("im1 %lux\n", inb(Int1aux));
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
		ur->ecode, ur->cs&0xff, ur->pc, ur->ss&0xff, ur->usp);

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
#include "../port/systab.h"

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
	ur->ax = ret;
	if(ax == NOTED){
		noted(ur, *(ulong*)(sp+BY2WD));
	} else if(u->nnote && ax!=FORK){
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
	u->p->notepending = 0;
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
	|| (nur->flags&0xff00)!=(u->svflags&0xff00)){
		pprint("bad noted ureg cs %ux ss %ux flags %ux\n", nur->cs, nur->ss,
			nur->flags);
    Die:
		pexit("Suicide", 0);
	}
	lock(&u->p->debug);
	if(!u->notified){
		pprint("call to noted() when not notified\n");
		unlock(&u->p->debug);
		return;
	}
	u->notified = 0;
	nur->flags = (u->svflags&0xffffff00) | (ur->flags&0xff);
	memmove(ur, u->ureg, sizeof(Ureg));
	switch(arg0){
	case NCONT:
		if(waserror()){
			pprint("suicide: trap in noted\n");
			unlock(&u->p->debug);
			goto Die;
		}
		validaddr(nur->pc, 1, 0);
		validaddr(nur->usp, BY2WD, 0);
		poperror();
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
