#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

#include	<libg.h>
#include	<gnot.h>

typedef struct Boot Boot;

struct Boot
{
	long station;
	long traffic;
	char user[NAMELEN];
	char server[64];
	char line[64];
	char device;
};
#define BOOT ((Boot*)0)

char	user[NAMELEN];
char bootuser[NAMELEN];
char bootline[64];
char bootserver[64];
char bootdevice[2];
int bank[2];

void unloadboot(void);

void
main(void)
{
	u = 0;
	unloadboot();
	machinit();
	mmuinit();
	confinit();
	kmapinit();
	duartinit();
	screeninit();
	printinit();
	print("bank 0: %dM  bank 1: %dM\n", bank[0], bank[1]);
	flushmmu();
	procinit0();
	initseg();
	grpinit();
	chaninit();
	alarminit();
	chandevreset();
	streaminit();
/*	serviceinit(); /**/
/*	filsysinit(); /**/
	swapinit();
	pageinit();
	kmapinit();
	userinit();
	schedinit();
}

void
unloadboot(void)
{
	strncpy(bootuser, BOOT->user, NAMELEN);
	memmove(bootline, BOOT->line, 64);
	memmove(bootserver, BOOT->server, 64);
	bootdevice[0] = BOOT->device;
}

void
machinit(void)
{
	int n;

	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->fpstate = FPinit;
	fprestore(&initfp);
}

void
mmuinit(void)
{
	ulong l, d, i;

	/*
	 * Invalidate user addresses
	 */
	for(l=0; l<4*1024*1024; l+=BY2PG)
		putmmu(l, INVALIDPTE, 0);
	/*
	 * Four meg of usable memory, with top 256K for screen
	 */
	for(i=1,l=KTZERO; i<(4*1024*1024-256*1024)/BY2PG; l+=BY2PG,i++)
		putkmmu(l, PPN(l)|PTEVALID|PTEKERNEL);
	/*
	 * Screen at top of memory
	 */
	for(i=0,d=DISPLAYRAM; i<256*1024/BY2PG; d+=BY2PG,l+=BY2PG,i++)
		putkmmu(l, PPN(d)|PTEVALID|PTEKERNEL);
}

void
init0(void)
{
	Chan *c;

	u->nerrlab = 0;
	m->proc = u->p;
	u->p->state = Running;
	u->p->mach = m;
	spllo();

	chandevinit();
	
	u->slash = (*devtab[0].attach)(0);
	u->dot = clone(u->slash, 0);
	if(!waserror()){
		c = namec("#e/bootuser", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootuser, strlen(bootuser), 0);
		close(c);
		c = namec("#e/bootline", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootline, 64, 0);
		close(c);
		c = namec("#e/bootserver", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootserver, strlen(bootuser), 0);
		close(c);
		c = namec("#e/bootdevice", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootdevice, 2, 0);
		close(c);
	}
	poperror();

	touser();
}

FPsave	initfp;

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
	strcpy(p->pgrp->user, "bootes");
	strcpy(user, "bootes");
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (ulong)init0;
	p->sched.sp = USERADDR+BY2PG-5*BY2WD;
	p->sched.sr = SUPER|SPL(0);
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

void
exit(void)
{
	int i;

	u = 0;
	spllo();
	while(consactive())
		for(i=0; i<1000; i++)
			;
	splhi();
	firmware();
}

banksize(int base)
{
	ulong va;

	if(&end > (int *)((KZERO|1024L*1024L)-BY2PG))
		return 0;
	va = UZERO;	/* user page 1 is free to play with */
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG, 0);
	*(ulong*)va = 0;	/* 0 at 0M */
	putmmu(va, PTEVALID|(base+1)*1024L*1024L/BY2PG, 0);
	*(ulong*)va = 1;	/* 1 at 1M */
	putmmu(va, PTEVALID|(base+4)*1024L*1024L/BY2PG, 0);
	*(ulong*)va = 4;	/* 4 at 4M */
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG, 0);
	if(*(ulong*)va == 0)
		return 16;
	putmmu(va, PTEVALID|(base+1)*1024L*1024L/BY2PG, 0);
	if(*(ulong*)va == 1)
		return 4;
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG, 0);
	if(*(ulong*)va == 4)
		return 1;
	return 0;
}

Conf	conf;

void
confinit(void)
{
	int mul;
	conf.nmach = 1;
	if(conf.nmach > MAXMACH)
		panic("confinit");
	bank[0] = banksize(0);
	bank[1] = banksize(16);
	conf.npage0 = (bank[0]*1024*1024)/BY2PG;
	conf.base0 = 0;
	conf.npage1 = (bank[1]*1024*1024)/BY2PG;
	conf.base1 = 16*1024*1024;
	conf.npage = conf.npage0+conf.npage1;
	conf.maxialloc = (4*1024*1024-256*1024-BY2PG);
	mul = 1 + (conf.npage1>0);
	conf.nproc = 50*mul;
	conf.nseg = conf.nproc*4;
	conf.npagetab = conf.nseg*2;
	conf.nswap = 4096;
	conf.nimage = 50;
	conf.npgrp = 20*mul;
	conf.nalarm = 1000;
	conf.nchan = 200*mul;
	conf.nenv = 100*mul;
	conf.nenvchar = 8000*mul;
	conf.npgenv = 200*mul;
	conf.nmtab = 50*mul;
	conf.nmount = 80*mul;
	conf.nmntdev = 10*mul;
	conf.nmntbuf = conf.nmntdev+5;
	conf.nmnthdr = 2*conf.nmntdev;
	conf.nstream = 40 + 32*mul;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 24 * conf.nstream;
	conf.nsrv = 16*mul;			/* was 32 */
	conf.nbitmap = 300*mul;
	conf.nbitbyte = 300*1024*mul;
	if(*(uchar*)MOUSE & (1<<4))
		conf.nbitbyte *= 2;	/* ldepth 1 */
	conf.nfont = 10*mul;
	conf.nurp = 32;
	conf.nasync = 1;
	conf.npipe = conf.nstream/2;
	conf.nservice = 3*mul;			/* was conf.nproc/5 */
	conf.nfsyschan = 31 + conf.nchan/20;
	conf.copymode = 0;		/* copy on write */
	conf.portispaged = 0;
	conf.cntrlp = 0;
}

/*
 *  set up floating point for a new process
 */
void
procsetup(Proc *p)
{
	long fpnull;

	fpnull = 0;
	splhi();
	m->fpstate = FPinit;
	p->fpstate = FPinit;
	fprestore((FPsave*)&fpnull);
	spllo();
}

/*
 * Save the part of the process state.
 */
void
procsave(uchar *state, int len)
{
	Balu *balu;

	if(len < sizeof(Balu))
		panic("save state too small");
	balu = (Balu *)state;
	fpsave(&u->fpsave);
	if(u->fpsave.type){
		if(u->fpsave.size > sizeof u->fpsave.junk)
			panic("fpsize %d max %d\n", u->fpsave.size, sizeof u->fpsave.junk);
		fpregsave(u->fpsave.reg);
		u->p->fpstate = FPactive;
		m->fpstate = FPdirty;
	}
	if(BALU->cr0 != 0xFFFFFFFF)	/* balu busy */
		memmove(balu, BALU, sizeof(Balu));
	else{
		balu->cr0 = 0xFFFFFFFF;
		BALU->cr0 = 0xFFFFFFFF;
	}
}

/*
 *  Restore what procsave() saves
 *
 *  Procsave() makes sure that what state points to is long enough
 */
void
procrestore(Proc *p, uchar *state)
{
	Balu *balu;

	balu = (Balu *)state;
	if(p->fpstate != m->fpstate){
		if(p->fpstate == FPinit){
			u->p->fpstate = FPinit;
			fprestore(&initfp);
			m->fpstate = FPinit;
		}else{
			fpregrestore(u->fpsave.reg);
			fprestore(&u->fpsave);
			m->fpstate = FPdirty;
		}
	}
	if(balu->cr0 != 0xFFFFFFFF)	/* balu busy */
		memmove(BALU, balu, sizeof balu);
}

void
buzz(int f, int d)
{
}

void
lights(int val)
{
}
