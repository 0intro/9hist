#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

int machtype;
uchar	*sp;	/* stack pointer for /boot */

void
main(void)
{
	ident();
	meminit();
	machinit();
	active.exiting = 0;
	active.machs = 1;
	confinit();
	screeninit();
	printinit();
	print("%ludK bytes of physical memory\n", (conf.base1 + conf.npage1*BY2PG)/1024);
	mmuinit();
	trapinit();
	mathinit();
	clockinit();
	faultinit();
	kbdinit();
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

void
ident(void)
{
	char *id = (char*)(ROMBIOS + 0xFF40);

	/* check for a safari (tres special) */
	if(strncmp(id, "AT&TNSX", 7) == 0)
		machtype = Attnsx;
	else
		machtype = At;
}

void
machinit(void)
{
	int n;

	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->mmask = 1<<m->machno;
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

	kproc("alarm", alarmkproc, 0);
	chandevinit();

	if(!waserror()){
		ksetpcinfo();
		ksetenv("cputype", "386");
		poperror();
	}
	touser(sp);
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	User *up;
	KMap *k;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = newegrp();
	p->fgrp = newfgrp();
	p->procmode = 0640;

	strcpy(p->text, "*init*");
	strcpy(p->user, eve);
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
	pg = newpage(1, 0, USTKTOP-BY2PG);
	segpage(s, pg);
	k = kmap(pg);
	bootargs(VA(k));
	kunmap(k);

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

uchar *
pusharg(char *p)
{
	int n;

	n = strlen(p)+1;
	sp -= n;
	memmove(sp, p, n);
	return sp;
}

void
bootargs(ulong base)
{
 	int i, ac;
	uchar *av[32];
	char *p, *pp;
	uchar **lsp;

	sp = (uchar*)base + BY2PG - MAXSYSARG*BY2WD;

	ac = 0;
	av[ac++] = pusharg("/386/9safari");
	av[ac++] = pusharg("-p");

	/* 4 byte word align stack */
	sp = (uchar*)((ulong)sp & ~3);

	/* build argc, argv on stack */
	sp -= (ac+1)*sizeof(sp);
	lsp = (uchar**)sp;
	for(i = 0; i < ac; i++)
		*lsp++ = av[i] + ((USTKTOP - BY2PG) - base);
	*lsp = 0;
	sp += (USTKTOP - BY2PG) - base - sizeof(ulong);
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
	 *  the last 256k belongs to the roms and other devices
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

	mul = 1;
	conf.nproc = 30 + i*5;
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
	conf.nsubfont = 30*mul;
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
	conf.nfloppy = 2;
	conf.nhard = 1;
	conf.dkif = 1;
	confinit1();
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
	sprint(note, "sys: fp: %s, status 0x%ux, pc 0x%lux", msg, status, ur->pc);
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
procsave(Proc *p)
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
procrestore(Proc *p)
{
}

void
firmware(void)
{
	panic("firmware");
}

/*
 *  the following functions all are slightly different from
 *  PC to PC.
 */

/* enable address bit 20 (extended memory) */
void
meminit(void)
{
	switch(machtype){
	case Attnsx:
		heada20();		/* via headland chip */
		break;
	case At:
		i8042a20();		/* via keyboard controller */
		break;
	}
}

/*
 *  reset the i387 chip
 */
void
exit(void)
{
	int i;

	u = 0;
	print("exiting\n");
	switch(machtype){
	case Attnsx:
		headreset();		/* via headland chip */
		break;
	case At:
		print("hit the button...");
		for(;;);
		putcr3(0);		/* crash and burn */
	}
}

/*
 *  set cpu speed
 *	0 == low speed
 *	1 == high speed
 */
int
cpuspeed(int speed)
{
	switch(machtype){
	case Attnsx:
		return pmucpuspeed(speed);
	default:
		return 0;
	}
}

/*
 *  f == frequency (Hz)
 *  d == duration (ms)
 */
void
buzz(int f, int d)
{
	switch(machtype){
	case Attnsx:
		pmubuzz(f, d);
		break;
	default:
		break;
	}
}

/*
 *  each bit in val stands for a light
 */
void
lights(int val)
{
	switch(machtype){
	case Attnsx:
		pmulights(val);
		break;
	default:
		break;
	}
}

/*
 *  power to serial port
 *	onoff == 1 means on
 *	onoff == 0 means off
 */
int
serial(int onoff)
{
	switch(machtype){
	case Attnsx:
		return pmuserial(onoff);
	default:
		return 0;
	}
}

/*
 *  power to modem
 *	onoff == 1 means on
 *	onoff == 0 means off
 */
int
modem(int onoff)
{
	switch(machtype){
	case Attnsx:
		return pmumodem(onoff);
	default:
		return 0;
	}
}
