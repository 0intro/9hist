#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"errno.h"

void	notify(Ureg*);
void	noted(Ureg**, ulong);
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
	static char buf[64];	/* BUG: not reentrant! */

	if(tbr < sizeof trapname/sizeof(char*))
		return trapname[tbr];
	if(tbr >= 130)
		sprint(buf, "trap instruction %d", tbr-128);
	else if(17<=tbr && tbr<=31)
		sprint(buf, "interrupt level %d", tbr-16);
	else switch(tbr){
	case 36:
		return "cp disabled";
	case 40:
		return "cp exception";
	case 128:
		return "syscall";
	case 129:
		return "breakpoint";
	default:
		sprint(buf, "unknown trap %d", tbr);
	}
    Return:
	return buf;
}

void
trap(Ureg *ur)
{
	int user, x;
	char buf[64];
	ulong tbr;

	if(u) {
		u->p->pc = ur->pc;		/* BUG */
		u->dbgreg = ur;
	}

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
			sccintr();
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
    Error:
		if(user){
			spllo();
			sprint(buf, "sys: %s pc=0x%lux", excname(tbr), ur->pc);
			if(tbr == 8)
				sprint(buf+strlen(buf), " FSR %lux", u->fpsave.fsr);
			postnote(u->p, 1, buf, NDebug);
		}else{
			print("kernel trap: %s pc=0x%lux\n", excname(tbr), ur->pc);
			dumpregs(ur);
			for(;;);
		}
	}
    Return:
	if(user) {
		notify(ur);
		if(u->p->fpstate == FPinactive) {
			restfpregs(&u->fpsave);
			u->p->fpstate = FPactive;
			ur->psr |= PSREF;
		}
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
	int i;
	extern ulong etext;

	if(u){
		i = 0;
		for(l=(ulong)&l; l<USERADDR+BY2PG; l+=4){
			v = *(ulong*)l;
			if(KTZERO < v && v < (ulong)&etext)
				print("%lux=%lux  ", l, v);
			++i;
			if((i&7) == 0)
				print("\n");
		}
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

void
notify(Ureg *ur)
{
	ulong s, sp;

	if(u->p->procctl)
		procctl(u->p);
	if(u->nnote == 0)
		return;

	s = spllo();		/* need to go low as may fault */
	lock(&u->p->debug);
	u->p->notepending = 0;
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
		u->svpsr = ur->psr;
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
		*(ulong*)(sp+1*BY2WD) = (ulong)u->ureg;	/* arg 1 is ureg* (compat) */
		ur->r7 = (ulong)u->ureg;		/* arg 1 is ureg* */
		*(ulong*)(sp+0*BY2WD) = 0;		/* arg 0 is pc */
		ur->usp = sp;
		ur->pc = (ulong)u->notify;
		ur->npc = (ulong)u->notify+4;
		u->notified = 1;
		u->nnote--;
		memmove(&u->lastnote, &u->note[0], sizeof(Note));
		memmove(&u->note[0], &u->note[1], u->nnote*sizeof(Note));
	}
	unlock(&u->p->debug);
	splx(s);
}

/*
 * Return user to state before notify()
 */
void
noted(Ureg **urp, ulong arg0)
{
	Ureg *nur;

	nur = u->ureg;
	if(nur->psr!=u->svpsr){
		pprint("bad noted ureg psr %lux\n", nur->psr);
    Die:
		pexit("Suicide", 0);
	}
	lock(&u->p->debug);
	if(!u->notified){
		unlock(&u->p->debug);
		pprint("call to noted() when not notified\n");
		goto Die;
	}
	u->notified = 0;
	memmove(*urp, u->ureg, sizeof(Ureg));
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
		splhi();
		unlock(&u->p->debug);
		rfnote(urp);
		break;
		/* never returns */

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

#include "../port/systab.h"

long
syscall(Ureg *aur)
{
	int i;
	long ret;
	ulong sp;
	ulong r7;
	Ureg *ur;
	char *msg;

	ur = aur;
	if(ur->psr & PSRPSUPER)
		panic("recursive system call");
	u->p->insyscall = 1;
	u->p->pc = ur->pc;

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
		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-(1+MAXSYSARG)*BY2WD))
			validaddr(sp, ((1+MAXSYSARG)*BY2WD), 0);
		u->p->psstate = sysctab[r7];
		ret = (*systab[r7])((ulong*)(sp+1*BY2WD));
		poperror();
	}
	ur->pc += 4;
	ur->npc = ur->pc+4;
	u->nerrlab = 0;
	if(u->p->procctl)
		procctl(u->p);

	u->p->insyscall = 0;
	u->p->psstate = 0;
	if(r7 == NOTED)	/* ugly hack */
		noted(&aur, *(ulong*)(sp+1*BY2WD));	/* doesn't return */

	splhi();
	if(r7!=FORK && (u->p->procctl || u->nnote)){
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

/* This routine must save the values of registers the user is not permitted to write
 * from devproc and the restore the saved values before returning
 */
void
setregisters(Ureg *xp, char *pureg, char *uva, int n)
{
	ulong psr;

	psr = xp->psr;
	memmove(pureg, uva, n);
	xp->psr = psr;
}
