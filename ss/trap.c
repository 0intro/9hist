#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"../port/error.h"

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
	"fp: disabled",
	"window overflow",
	"window underflow",
	"unaligned address",
	"fp: exception",
	"data access exception",
	"tag overflow",
};

char*
excname(ulong tbr)
{
	static char buf[64];	/* BUG: not reentrant! */
	char xx[64];
	char *t;

	switch(tbr){
		return "trap: cp disabled";
	case 40:
		return "trap: cp exception";
	case 128:
		return "syscall";
	case 129:
		return "breakpoint";
	}
	if(tbr < sizeof trapname/sizeof(char*))
		t = trapname[tbr];
	else{
		if(tbr >= 130)
			sprint(xx, "trap instruction %d", tbr-128);
		else if(17<=tbr && tbr<=31)
			sprint(xx, "interrupt level %d", tbr-16);
		else
			sprint(xx, "unknown trap %d", tbr);
		t = xx;
	}
	if(strncmp(t, "fp: ", 4) == 0)
		strcpy(buf, t);
	else
		sprint(buf, "trap: %s", t);
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
			sprint(buf, "sys: %s", excname(tbr));
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
print("no dumpstack\n");
return;
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

int
notify(Ureg *ur)
{
	int l, sent;
	ulong s, sp;
	Note *n;

	if(u->p->procctl)
		procctl(u->p);
	if(u->nnote == 0)
		return 0;

	s = spllo();
	qlock(&u->p->debug);
	u->p->notepending = 0;
	n = &u->note[0];
	if(strncmp(n->msg, "sys:", 4) == 0){
		l = strlen(n->msg);
		if(l > ERRLEN-15)	/* " pc=0x12345678\0" */
			l = ERRLEN-15;
		sprint(n->msg+l, " pc=0x%.8lux", ur->pc);
	}
	if(n->flag!=NUser && (u->notified || u->notify==0)){
		if(u->note[0].flag == NDebug)
			pprint("suicide: %s\n", n->msg);
    Die:
		qunlock(&u->p->debug);
		pexit(n->msg, n->flag!=NDebug);
	}
	sent = 0;
	if(!u->notified){
		if(!u->notify)
			goto Die;
		sent = 1;
		u->svpsr = ur->psr;
		sp = ur->usp;
		sp -= sizeof(Ureg);
		if(!okaddr((ulong)u->notify, 1, 0)
		|| !okaddr(sp-ERRLEN-3*BY2WD, sizeof(Ureg)+ERRLEN-3*BY2WD, 0)){
			pprint("suicide: bad address in notify\n");
			qunlock(&u->p->debug);
			pexit("Suicide", 0);
		}
		u->ureg = (void*)sp;
		memmove((Ureg*)sp, ur, sizeof(Ureg));
		sp -= ERRLEN;
		memmove((char*)sp, u->note[0].msg, ERRLEN);
		sp -= 3*BY2WD;
		*(ulong*)(sp+2*BY2WD) = sp+3*BY2WD;	/* arg 2 is string */
		*(ulong*)(sp+1*BY2WD) = (ulong)u->ureg;	/* arg 1 is ureg* (compat) */
		u->svr7 = ur->r7;			/* save away r7 */
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
	qunlock(&u->p->debug);
	splx(s);
	return sent;
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
	qlock(&u->p->debug);
	if(!u->notified){
		qunlock(&u->p->debug);
		pprint("call to noted() when not notified\n");
		goto Die;
	}
	u->notified = 0;
	memmove(*urp, u->ureg, sizeof(Ureg));
	(*urp)->r7 = u->svr7;
	switch(arg0){
	case NCONT:
		if(!okaddr(nur->pc, 1, 0) || !okaddr(nur->usp, BY2WD, 0)){
			pprint("suicide: trap in noted\n");
			qunlock(&u->p->debug);
			goto Die;
		}
		splhi();
		qunlock(&u->p->debug);
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
		qunlock(&u->p->debug);
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
	Ureg *ur;

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

	u->scallnr = ur->r7;
	sp = ur->usp;

	u->nerrlab = 0;
	ret = -1;
	if(!waserror()){
		if(u->scallnr >= sizeof systab/BY2WD){
			pprint("bad sys call number %d pc %lux\n", u->scallnr, ur->pc);
			postnote(u->p, 1, "sys: bad sys call", NDebug);
			error(Ebadarg);
		}

		if(sp & (BY2WD-1)){
			pprint("odd sp in sys call pc %lux sp %lux\n", ur->pc, ur->sp);
			postnote(u->p, 1, "sys: odd stack", NDebug);
			error(Ebadarg);
		}

		if(sp<(USTKTOP-BY2PG) || sp>(USTKTOP-sizeof(Sargs)))
			validaddr(sp, sizeof(Sargs), 0);

		u->s = *((Sargs*)(sp+1*BY2WD));
		u->p->psstate = sysctab[u->scallnr];

		ret = (*systab[u->scallnr])(u->s.args);
		poperror();
	}

	ur->pc += 4;
	ur->npc = ur->pc+4;
	u->nerrlab = 0;
	if(u->p->procctl)
		procctl(u->p);

	u->p->insyscall = 0;
	u->p->psstate = 0;
	if(u->scallnr == NOTED)	/* ugly hack */
		noted(&aur, *(ulong*)(sp+1*BY2WD));	/* doesn't return */

	splhi();
	if(u->scallnr!=RFORK && (u->p->procctl || u->nnote)){
		ur->r7 = ret;				/* load up for noted() */
		if(notify(ur))
			return ur->r7;
	}
	return ret;
}

long
execregs(ulong entry, ulong ssize, ulong nargs)
{
	ulong *sp;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;
	((Ureg*)UREGADDR)->usp = (ulong)sp;
	((Ureg*)UREGADDR)->pc = entry - 4;	/* syscall advances it */
	return USTKTOP-BY2WD;			/* address of user-level clock */
}

ulong
userpc(void)
{
	return ((Ureg*)UREGADDR)->pc;
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
