#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"../port/error.h"

enum 
{
	Maxhandler=	32+16,		/* max number of interrupt handlers */
};

typedef struct Handler	Handler;
struct Handler
{
	void	(*r)(Ureg*, void*);
	void	*arg;
	Handler	*next;
	int	edge;
	int	nintr;
	ulong	ticks;
	int	maxtick;
};

struct
{
	Handler	*ivec[128];
	Handler	h[Maxhandler];
	int	free;
} halloc;

void	faultpower(Ureg *ur, ulong addr, int read);
void	kernfault(Ureg*, int);

char *excname[] =
{
	"reserved 0",
	"system reset",
	"machine check",
	"data access",
	"instruction access",
	"external interrupt",
	"alignment",
	"program exception",
	"floating-point unavailable",
	"decrementer",
	"i/o controller interface error",
	"reserved B",
	"system call",
	"trace trap",
	"floating point assist",
	"reserved F",
	"software emulation",
	"ITLB miss",
	"DTLB miss",
	"ITLB error",
	"DTLB error",
	"reserved 15",
	"reserved 16",
	"reserved 17",
	"reserved 18",
	"reserved 19",
	"reserved 1A",
	"reserved 1B",
	"data breakpoint",
	"instruction breakpoint",
	"peripheral breakpoint",
	"development port",
	/* the following are made up on a program exception */
	"floating point exception",		/* 20: FPEXC */
	"illegal instruction",	/* 21 */
	"privileged instruction",	/* 22 */
	"trap",	/* 23 */
	"illegal operation",	/* 24 */
	"breakpoint",	/* 25 */
};

char *fpcause[] =
{
	"inexact operation",
	"division by zero",
	"underflow",
	"overflow",
	"invalid operation",
};
char	*fpexcname(Ureg*, ulong, char*);
#define FPEXPMASK	0xfff80300		/* Floating exception bits in fpscr */


char *regname[]={
	"CAUSE",	"SRR1",
	"PC",		"GOK",
	"LR",		"CR",
	"XER",	"CTR",
	"R0",		"R1",
	"R2",		"R3",
	"R4",		"R5",
	"R6",		"R7",
	"R8",		"R9",
	"R10",	"R11",
	"R12",	"R13",
	"R14",	"R15",
	"R16",	"R17",
	"R18",	"R19",
	"R20",	"R21",
	"R22",	"R23",
	"R24",	"R25",
	"R26",	"R27",
	"R28",	"R29",
	"R30",	"R31",
};

void
sethvec(int v, void (*r)(void))
{
	ulong *vp, pa, o;

	vp = (ulong*)(0xfff00000+v);
	vp[0] = 0x7c1043a6;	/* MOVW R0, SPR(SPRG0) */
	vp[1] = 0x7c0802a6;	/* MOVW LR, R0 */
	vp[2] = 0x7c1243a6;	/* MOVW R0, SPR(SPRG2) */
	pa = PADDR(r);
	o = pa >> 25;
	if(o != 0 && o != 0x7F){
		/* a branch too far */
		vp[3] = (15<<26)|(pa>>16);	/* MOVW $r&~0xFFFF, R0 */
		vp[4] = (24<<26)|(pa&0xFFFF);	/* OR $r&0xFFFF, R0 */
		vp[5] = 0x7c0803a6;	/* MOVW	R0, LR */
		vp[6] = 0x4e800021;	/* BL (LR) */
	}else
		vp[3] = (18<<26)|(pa&0x3FFFFFC)|3;	/* bla */
	dcflush(vp, 8*sizeof(ulong));
}

void
sethvec2(int v, void (*r)(void))
{
	ulong *vp;

	vp = (ulong*)KADDR(v);
	vp[0] = (18<<26)|(PADDR(r))|2;	/* ba */
	dcflush(vp, sizeof(*vp));
}

void
trap(Ureg *ur)
{
	int ecode;
	static struct {int callsched;} c = {1};

	ecode = (ur->cause >> 8) & 0xff;
	if(ecode < 0 || ecode >= 0x1F)
		ecode = 0x1F;
	switch(ecode){
	case CEI:
		intr(ur);
		break;

	case CDEC:
		clockintr(ur);
		break;

	case CIMISS:
	case CITLBE:
		faultpower(ur, ur->pc, 1);
		break;
	case CDMISS:
		faultpower(ur, getdepn(), 1);
		break;
	case CDTLBE:
		faultpower(ur, getdar(), !(getdsisr() & (1<<25)));
		break;

	case CEMU:
		print("pc=#%lux op=#%8.8lux\n", ur->pc, *(ulong*)ur->pc);
		goto Default;
		break;

	case CSYSCALL:
		spllo();
		print("syscall!\n");
		dumpregs(ur);
		delay(100);
		splhi();
		break;

	case CPROG:
		if(ur->status & (1<<19))
			ecode = 0x20;
		if(ur->status & (1<<18))
			ecode = 0x21;
		if(ur->status & (1<<17))
			ecode = 0x22;
		goto Default;

	Default:
	default:
		print("kernel %s pc=0x%lux\n", excname[ecode], ur->pc);
		dumpregs(ur);
		dumpstack();
		spllo();
		exit(1);
	}

	splhi();
}

void
faultpower(Ureg *ureg, ulong addr, int read)
{
	int user, insyscall, n;
	char buf[ERRLEN];

	user = (ureg->srr1 & MSR_PR) != 0;
print("fault: pc = %ux, addr = %ux read = %d user = %d stack=%ux\n", ureg->pc, addr, read, user, &ureg);
	insyscall = up->insyscall;
	up->insyscall = 1;
	spllo();
	n = fault(addr, read);
	if(n < 0){
		if(!user){
			dumpregs(ureg);
			panic("fault: 0x%lux\n", addr);
		}
		sprint(buf, "sys: trap: fault %s addr=0x%lux",
			read? "read" : "write", addr);
		postnote(up, 1, buf, NDebug);
	}
	up->insyscall = insyscall;
	splhi();
}

void
spurious(Ureg *ur, void *a)
{
	USED(a);
	print("SPURIOUS interrupt pc=0x%lux cause=0x%lux\n",
		ur->pc, ur->cause);
	panic("bad interrupt");
}

#define	LEV(n)	(((n)<<1)|1)
#define	IRQ(n)	(((n)<<1)|0)

Lock	veclock;

void
trapinit(void)
{
	int i;
	IMM *io;


	io = m->iomem;
	io->sypcr &= ~(1<<3);	/* disable watchdog (821/823) */
	io->simask = 0;	/* mask all */
	io->siel = ~0;	/* edge sensitive, wake on all */
	io->cicr = 0;	/* disable CPM interrupts */
	io->cipr = ~0;	/* clear all interrupts */
	io->cimr = 0;	/* mask all events */
	io->cicr = (0xE1<<16)|(CPIClevel<<13)|(0x1F<<8);
	io->cicr |= 1 << 7;	/* enable */
	io->tbscr = 1;	/* TBE */
	io->simask |= 1<<(31-LEV(CPIClevel));	/* CPM's level */
	eieio();
	putdec(~0);

	/*
	 * set all exceptions to trap
	 */
	for(i = 0x0; i < 0x3000; i += 0x100)
		sethvec(i, trapvec);

	//sethvec(CEI<<8, intrvec);
	//sethvec2(CIMISS<<8, itlbmiss);
	//sethvec2(CDMISS<<8, dtlbmiss);
}

void
intrenable(int v, void (*r)(Ureg*, void*), void *arg, int)
{
	Handler *h;
	IMM *io;

	if(halloc.free >= Maxhandler)
		panic("out of interrupt handlers");
	v -= VectorPIC;
	if(v < 0 || v >= nelem(halloc.ivec))
		panic("intrenable(%d)", v+VectorPIC);
	ilock(&veclock);
	if(v < VectorCPIC || (h = halloc.ivec[v]) == nil){
		h = &halloc.h[halloc.free++];
		h->next = halloc.ivec[v];
		halloc.ivec[v] = h;
	}
	h->r = r;
	h->arg = arg;

	/*
	 * enable corresponding interrupt in SIU/CPM
	 */

	eieio();
	io = m->iomem;
	if(v >= VectorCPIC){
		v -= VectorCPIC;
		io->cimr |= 1<<(v&0x1F);
	}
	else if(v >= VectorIRQ)
		io->simask |= 1<<(31-IRQ(v&7));
	else
		io->simask |= 1<<(31-LEV(v));
	eieio();
	iunlock(&veclock);
}

/*
 * called directly by l.s:/intrvec
 */
void
intr(Ureg *ur)
{
	int b, v;
	IMM *io;
	Handler *h;
	long t0;

	ur->cause &= ~0xff;
	io = m->iomem;
	b = io->sivec>>2;
	v = b>>1;
	if(b & 1) {
		if(v == CPIClevel){
			io->civr = 1;
			eieio();
			v = VectorCPIC+(io->civr>>11);
		}
	}else
		v += VectorIRQ;
	ur->cause |= v;
	h = halloc.ivec[v];
	if(h == nil){
		print("unknown interrupt %d pc=0x%lux\n", v, ur->pc);
		uartwait();
		return;
	}
	if(h->edge){
		io->sipend |= 1<<(31-b);
		eieio();
	}
	/*
	 *  call the interrupt handlers
	 */
	do {
		h->nintr++;
		t0 = getdec();
		(*h->r)(ur, h->arg);
		t0 -= getdec();
		h->ticks += t0;
		if(h->maxtick < t0)
			h->maxtick = t0;
		h = h->next;
	} while(h != nil);
	if(v >= VectorCPIC)
		io->cisr |= 1<<(v-VectorCPIC);
	eieio();
}

int
intrstats(char *buf, int bsize)
{
	Handler *h;
	int i, n;

	n = 0;
	for(i=0; i<nelem(halloc.ivec) && n < bsize; i++)
		if((h = halloc.ivec[i]) != nil && h->nintr)
			n += snprint(buf+n, bsize-n, "%3d %lud %lud %lud\n", i, h->nintr, h->ticks, h->maxtick);
	return n;
}

char*
fpexcname(Ureg *ur, ulong fpscr, char *buf)
{
	int i;
	char *s;
	ulong fppc;

	fppc = ur->pc;
	s = 0;
	fpscr >>= 3;		/* trap enable bits */
	fpscr &= (fpscr>>22);	/* anded with exceptions */
	for(i=0; i<5; i++)
		if(fpscr & (1<<i))
			s = fpcause[i];
	if(s == 0)
		return "no floating point exception";
	sprint(buf, "%s fppc=0x%lux", s, fppc);
	return buf;
}

#define KERNPC(x)	(KTZERO<(ulong)(x)&&(ulong)(x)<(ulong)etext)

void
kernfault(Ureg *ur, int code)
{
	Label l;

	print("panic: kfault %s dar=0x%lux\n", excname[code], getdar());
	print("u=0x%lux status=0x%lux pc=0x%lux sp=0x%lux\n",
				up, ur->status, ur->pc, ur->sp);
	dumpregs(ur);
	l.sp = ur->sp;
	l.pc = ur->pc;
	dumpstack();
	for(;;)
		sched();
}

void
dumpstack(void)
{
	ulong l, v;
	int i;

	if(up == 0)
		return;
	i = 0;
	for(l=(ulong)&l; l<(ulong)(up->kstack+KSTACK); l+=4){
		v = *(ulong*)l;
		if(KTZERO < v && v < (ulong)etext){
			print("%lux=%lux, ", l, v);
			if(i++ == 4){
				print("\n");
				i = 0;
			}
		}
	}
}

void
dumpregs(Ureg *ur)
{
	int i;
	ulong *l;
	if(up) {
		print("registers for %s %d\n", up->text, up->pid);
		if(ur->srr1 & MSR_PR == 0)
		if(ur->usp < (ulong)up->kstack || ur->usp > (ulong)up->kstack+KSTACK)
			print("invalid stack ptr\n");
	}
	else
		print("registers for kernel\n");

	print("dsisr\t%.8lux\tdar\t%.8lux\n", getdsisr(), getdar());
	l = &ur->cause;
	for(i=0; i<sizeof regname/sizeof(char*); i+=2, l+=2)
		print("%s\t%.8lux\t%s\t%.8lux\n", regname[i], l[0], regname[i+1], l[1]);
}

static void
linkproc(void)
{
print("linkproc %ux\n", up);
	spllo();
	(*up->kpfun)(up->kparg);
	pexit("", 0);
}

void
kprocchild(Proc *p, void (*func)(void*), void *arg)
{
	p->sched.pc = (ulong)linkproc;
	p->sched.sp = (ulong)p->kstack+KSTACK;

	p->kpfun = func;
	p->kparg = arg;
}

int
isvalid_pc(ulong pc)
{
	return KERNPC(pc) && (pc&3) == 0;
}

void
dumplongs(char*, ulong*, int)
{
}

void
reset(void)
{
	IMM *io;

	io = m->iomem;
	io->plprcrk = KEEP_ALIVE_KEY;	// unlock
	io->plprcr |= IBIT(24);		// enable checkstop reset
	putmsr(getmsr() & ~MSR_ME);
	// cause checkstop -> causes reset
	*(ulong*)(0) = 0;
}

/*
 * called in sysfile.c
 */
void
evenaddr(ulong addr)
{
	if(addr & 3){
		postnote(up, 1, "sys: odd address", NDebug);
		error(Ebadarg);
	}
}

long
execregs(ulong entry, ulong ssize, ulong nargs)
{
	ulong *sp;
	Ureg *ureg;

	sp = (ulong*)(USTKTOP - ssize);
	*--sp = nargs;

	ureg = up->dbgreg;
	ureg->usp = (ulong)sp;
	ureg->pc = entry;
	return USTKTOP-BY2WD;		/* address of user-level clock */
}

void
forkchild(Proc *p, Ureg *ur)
{
	Ureg *cur;

	p->sched.sp = (ulong)p->kstack+KSTACK-UREGSIZE;
	p->sched.pc = (ulong)forkret;

	cur = (Ureg*)(p->sched.sp+2*BY2WD);
	memmove(cur, ur, sizeof(Ureg));

	cur->pc += 4;

	/* Things from bottom of syscall we never got to execute */
	p->psstate = 0;
	p->insyscall = 0;
}

ulong
userpc(void)
{
	Ureg *ureg;

	ureg = (Ureg*)up->dbgreg;
	return ureg->pc;
}


// need to be in l.s
void
forkret(void)
{
	panic("forkret");
}
/* This routine must save the values of registers the user is not 
 * permitted to write from devproc and then restore the saved values 
 * before returning
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
setkernur(Ureg* ureg, Proc* p)
{
	ureg->pc = p->sched.pc;
	ureg->sp = p->sched.sp+4;
}

ulong
dbgpc(Proc *p)
{
	Ureg *ureg;

	ureg = p->dbgreg;
	if(ureg == 0)
		return 0;

	return ureg->pc;
}
