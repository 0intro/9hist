#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

extern long edata;

char	user[NAMELEN] = "bootes";

void
main(void)
{
	meminit();
	machinit();
	confinit();
	screeninit();
	printinit();
	print("%ludK bytes of physical memory\n", (conf.base1 + conf.npage1*BY2PG)/1024);
	mmuinit();
	trapinit();
	mathinit();
	kbdinit();
	clockinit();
	faultinit();
	procinit0();
	initseg();
	grpinit();
	chaninit();
	alarminit();
	chandevreset();
	streaminit();
	swapinit();
	pageinit();
	userinit();

	schedinit();
}

/*
 *	BUG -- needs floating point support
 */
void
machinit(void)
{
	int n;

	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->mmask = 1<<m->machno;
	active.machs = 1;
}

ulong garbage;

void
init0(void)
{
	Chan *c;

	u->nerrlab = 0;
	m->proc = u->p;
	u->p->state = Running;
	u->p->mach = m;

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	u->slash = (*devtab[0].attach)(0);
	u->dot = clone(u->slash, 0);

	chandevinit();

	if(!waserror()){
		ksetterm("at&t %s");
		ksetenv("cputype", "386");
		poperror();
	}
	touser();
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	User *up;
	KMap *k;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = newegrp();
	p->fgrp = newfgrp();

	strcpy(p->text, "*init*");
	strcpy(p->user, user);
	p->fpstate = FPinit;
	fpoff();

	/*
	 * Kernel Stack
	 *
	 * N.B. The -12 for the stack pointer is important.
	 *	4 bytes for gotolabel's return PC
	 */
	p->sched.pc = (ulong)init0;
	p->sched.sp = USERADDR + BY2PG - 4;
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));

	/*
	 * User
	 */
	k = kmap(p->upage);
	up = (User*)VA(k);
	up->p = p;
	kunmap(k);

	/*
	 * User Stack
	 */
	s = newseg(SG_STACK, USTKTOP-BY2PG, 1);
	p->seg[SSEG] = s;

	/*
	 * Text
	 */
	s = newseg(SG_TEXT, UTZERO, 1);
	p->seg[TSEG] = s;
	segpage(s, newpage(1, 0, UTZERO));
	k = kmap(s->map[0]->pages[0]);
	memmove((ulong*)VA(k), initcode, sizeof initcode);
	kunmap(k);

	ready(p);
}

Conf	conf;

void
confinit(void)
{
	long x, i, j, *l;
	int mul;

	/*
	 *  the first 640k is the standard useful memory
	 *  the next 128K is the display
	 *  the last 256k belongs to the roms
	 */
	conf.npage0 = 640/4;
	conf.base0 = 0;

	/*
	 *  size the non-standard memory
	 */
	x = 0x12345678;
	for(i=1; i<16; i++){
		/*
		 *  write the word
		 */
		l = (long*)(KZERO|(i*1024L*1024L));
		*l = x;
		/*
		 *  take care of wraps
		 */
		for(j = 0; j < i; j++){
			l = (long*)(KZERO|(j*1024L*1024L));
			*l = 0;
		}
		/*
		 *  check
		 */
		l = (long*)(KZERO|(i*1024L*1024L));
		if(*l != x)
			break;
		x += 0x3141526;
	}
	conf.base1 = 0x100000;
	conf.npage1 = ((i-1)*1024*1024 - conf.base1)/BY2PG;

	conf.npage = conf.npage0 + conf.npage1;
	conf.maxialloc = 2*1024*1024;

	mul = 1;
	conf.nproc = 30 + 20*mul;
	conf.npgrp = conf.nproc/2;
	conf.nseg = conf.nproc*3;
	conf.npagetab = (conf.nseg*14)/10;
	conf.nswap = conf.nproc*80;
	conf.nimage = 50;
	conf.nalarm = 1000;
	conf.nchan = 6*conf.nproc;
	conf.nenv = 4*conf.nproc;
	conf.nenvchar = 8000*mul;
	conf.npgenv = 200*mul;
	conf.nmtab = 50*mul;
	conf.nmount = 80*mul;
	conf.nmntdev = 15*mul;
	conf.nmntbuf = conf.nmntdev+3;
	conf.nmnthdr = 2*conf.nmntdev;
	conf.nsrv = 16*mul;			/* was 32 */
	conf.nbitmap = 512*mul;
	conf.nbitbyte = conf.nbitmap*1024*screenbits();
	conf.nfont = 10*mul;
	conf.nnoifc = 1;
	conf.nnoconv = 32;
	conf.nurp = 32;
	conf.nasync = 1;
	conf.nstream = (conf.nproc*3)/2;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 24 * conf.nstream;
	conf.npipe = conf.nstream/2;
	conf.copymode = 0;			/* copy on write */
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;
	conf.cntrlp = 0;
	conf.nfloppy = 1;
	conf.nhard = 1;
}

char *mathmsg[] =
{
	"invalid",
	"denormalized",
	"div-by-zero",
	"overflow",
	"underflow",
	"precision",
	"stack",
	"error",
};

/*
 *  math coprocessor error
 */
void
matherror(Ureg *ur)
{
	ulong status;
	int i;
	char *msg;
	char note[ERRLEN];

	/*
	 *  a write cycle to port 0xF0 clears the interrupt latch attached
	 *  to the error# line from the 387
	 */
	outb(0xF0, 0xFF);

	status = fpstatus() & 0xffff;
	msg = "unknown";
	for(i = 0; i < 8; i++)
		if((1<<i) & status){
			msg = mathmsg[i];
			break;
		}
	sprint(note, "math: %s, status 0x%ux, pc 0x%lux", msg, status, ur->pc);
	postnote(u->p, 1, note, NDebug);
}

/*
 *  math coprocessor emulation fault
 */
void
mathemu(Ureg *ur)
{
	switch(u->p->fpstate){
	case FPinit:
		fpinit();
		u->p->fpstate = FPactive;
		break;
	case FPinactive:
		fprestore(&u->fpsave);
		u->p->fpstate = FPactive;
		break;
	case FPactive:
		pexit("Math emu", 0);
		break;
	}
}

/*
 *  math coprocessor segment overrun
 */
void
mathover(Ureg *ur)
{
	pexit("Math overrun", 0);
}

void
mathinit(void)
{
	setvec(Matherr1vec, matherror);
	setvec(Matherr2vec, matherror);
	setvec(Mathemuvec, mathemu);
	setvec(Mathovervec, mathover);
}

/*
 *  set up floating point for a new process
 */
void
procsetup(Proc *p)
{
	p->fpstate = FPinit;
	fpoff();
}

/*
 *  Save the mach dependent part of the process state.
 */
void
procsave(uchar *state, int len)
{
	if(u->p->fpstate == FPactive){
		fpsave(&u->fpsave);
		u->p->fpstate = FPinactive;
	}
}

/*
 *  Restore what procsave() saves
 */
void
procrestore(Proc *p, uchar *state)
{
}

void
firmware(void)
{
	panic("firmware");
}

/*
 *  special stuff for 80c51 power management and headland system controller
 */
enum
{
	/*
	 *  system control port
	 */
	Head=		0x92,		/* control port */
	 Reset=		(1<<0),		/* reset the 386 */
	 A20ena=	(1<<1),		/* enable address line 20 */

	/*
	 *  power management unit ports
	 */
	Pmudata=	0x198,

	Pmucsr=		0x199,
	 Busy=		0x1,

	/*
	 *  configuration port
	 */
	Pconfig=	0x3F3,
};

/*
 *  enable address bit 20
 */
void
meminit(void)
{
	outb(Head, A20ena);		/* enable memory address bit 20 */
}

/*
 *  reset the chip
 */
void
exit(void)
{
	int i;

	u = 0;
	print("exiting\n");
	outb(Head, Reset);
}

/*
 *  return when pmu ready
 */
static int
pmuready(void)
{
	int tries;

	for(tries = 0; (inb(Pmucsr) & Busy); tries++)
		if(tries > 1000)
			return -1;
	return 0;
}

/*
 *  return when pmu busy
 */
static int
pmubusy(void)
{
	int tries;

	for(tries = 0; !(inb(Pmucsr) & Busy); tries++)
		if(tries > 1000)
			return -1;
	return 0;
}

/*
 *  set a bit in the PMU
 */
Lock pmulock;
int
pmuwrbit(int index, int bit, int pos)
{
	lock(&pmulock);
	outb(Pmucsr, 0x02);		/* next is command request */
	if(pmuready() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmudata, (2<<4) | index);	/* send write bit command */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmucsr, 0x01);		/* next is data */
	if(pmuready() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmudata, (bit<<3) | pos);	/* send bit to write */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0){
		unlock(&pmulock);
		return -1;
	}
	unlock(&pmulock);
	return 0;
}

/*
 *  power to serial port
 *	onoff == 0 means on
 *	onoff == 1 means off
 */
int
serial(int onoff)
{
	return pmuwrbit(1, onoff, 6);
}

/*
 *  power to modem
 *	onoff == 0 means on
 *	onoff == 1 means off
 */
int
modem(int onoff)
{
	if(pmuwrbit(1, onoff, 0)<0)
		return -1;
	return pmuwrbit(1, 1^onoff, 5);
}

/*
 *  CPU speed
 *	onoff == 0 means 2 MHZ
 *	onoff == 1 means 20 MHZ
 */
int
cpuspeed(int speed)
{
	return pmuwrbit(0, speed, 0);
}

void
buzz(int f, int d)
{
	static Rendez br;
	static QLock bl;

	qlock(&bl);
	pmuwrbit(0, 0, 6);
	tsleep(&br, return0, 0, d);
	pmuwrbit(0, 1, 6);
	qunlock(&bl);
}

void
lights(int val)
{
	static QLock ll;

	qlock(&ll);
	pmuwrbit(0, (val&1), 4);		/* owl */
	pmuwrbit(0, ((val>>1)&1), 1);		/* mail */
	qunlock(&ll);
}
