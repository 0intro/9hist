#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

char	user[NAMELEN] = "bootes";
extern long edata;

void
main(void)
{
	a20enable();
	machinit();
	confinit();
	screeninit();
	printinit();
	mmuinit();
	trapinit();
	kbdinit();
	clockinit();
	faultinit();
	procinit0();
	initseg();
	grpinit();
	chaninit();
	alarminit();
print("chandevreset\n"); delay(1000);
	chandevreset();
print("streaminit\n"); delay(1000);
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
	m->fpstate = FPinit;
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
	p->fpstate = FPinit;

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
	 *  size memory
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

	/*
	 *  the first 640k is the standard useful memory
	 */
	conf.npage0 = 640/4;
	conf.base0 = 0;

	/*
	 *  the last 128k belongs to the roms
	 */
	conf.npage1 = (i)*1024/4;
	conf.base1 = 1024*1024;

	conf.npage = conf.npage0 + conf.npage1;
	conf.maxialloc = 2*1024*1024;

	mul = 1;
	conf.nproc = 20 + 20*mul;
	conf.npgrp = conf.nproc/2;
	conf.nseg = conf.nproc*3;
	conf.npagetab = (conf.nseg*14)/10;
	conf.nswap = conf.nproc*80;
	conf.nimage = 50;
	conf.nalarm = 1000;
	conf.nchan = 4*conf.nproc;
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
}

/*
 *  set up floating point for a new process
 *	BUG -- needs floating point support
 */
void
procsetup(Proc *p)
{
	p->fpstate = FPinit;
	m->fpstate = FPinit;
}

/*
 * Save the part of the process state.
 *	BUG -- needs floating point support
 */
void
procsave(uchar *state, int len)
{
}

/*
 *  Restore what procsave() saves
 *	BUG -- needs floating point support
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


void
buzz(int f, int d)
{
}

void
lights(int val)
{
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
	 Busy=	0x1,
};

/*
 *  enable address bit 20
 */
void
a20enable(void)
{
	outb(Head, A20ena);
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
int
pmuwrbit(int index, int bit, int pos)
{
	outb(Pmucsr, 0x02);		/* next is command request */
	if(pmuready() < 0)
		return -1;
	outb(Pmudata, (2<<4) | index);	/* send write bit command */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0)
		return -1;
	outb(Pmucsr, 0x01);		/* next is data */
	if(pmuready() < 0)
		return -1;
	outb(Pmudata, (bit<<3) | pos);	/* send bit to write */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0)
		return -1;
}

/*
 *  control power to the serial line
 *	onoff == 0 means turn power on
 *	onoff == 1 means off
 */
int
serial(int onoff)
{
	return pmuwrbit(1, onoff, 6);
}

int
owl(int onoff)
{
	return pmuwrbit(0, onoff, 4);
}

int
mail(int onoff)
{
	return pmuwrbit(0, onoff, 1);
}
