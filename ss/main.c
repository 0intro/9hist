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

char user[NAMELEN];

uchar *intrreg;

void
main(void)
{
	int a;

	u = 0;
	memset(&edata, 0, (char*)&end-(char*)&edata);

	machinit();
	confinit();
	mmuinit();
	screeninit();
	printinit();
	print("sparc plan 9\n");
	trapinit();
	kmapinit();
	ioinit();
	cacheinit();
	intrinit();
	procinit0();
	initseg();
	grpinit();
	chaninit();
	alarminit();
	chandevreset();
	streaminit();
/*	serviceinit(); /**/
/*	filsysinit(); /**/
	pageinit();
	userinit();
	clockinit();
	schedinit();
}

void
intrinit(void)
{
	KMap *k;

	k = kmappa(INTRREG, PTEIO);
	intrreg = (uchar*)k->va;
}

void
systemreset(void)
{
	delay(100);
	putb2(ENAB, ENABRESET);
}


void
machinit(void)
{
	int n;

	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->mmask = 1<<m->machno;
	m->fpstate = FPinit;
}

void
ioinit(void)
{
	KMap *k;

	/* tell scc driver it's address */
	k = kmappa(KMDUART, PTEIO|PTENOCACHE);
	sccsetup((void*)(k->va));

	/* scc port 0 is the keyboard */
	sccspecial(0, 0, &kbdq, 2400);
	kbdq.putc = kbdstate;

	/* scc port 1 is the mouse */
	sccspecial(1, 0, &mouseq, 2400);
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
		ksetterm("sun %s");
		ksetenv("cputype", "sparc");
		poperror();
	}

	touser(USTKTOP-(1+MAXSYSARG)*BY2WD);
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
	savefpregs(&initfp);
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (((ulong)init0) - 8);	/* 8 because of RETURN in gotolabel */
	p->sched.sp = USERADDR+BY2PG-(1+MAXSYSARG)*BY2WD;
	p->upage = newpage(0, 0, USERADDR|(p->pid&0xFFFF));

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
	print("cpu %d exiting\n", m->machno);
	while(consactive())
		for(i=0; i<1000; i++)
			;
	splhi();
	systemreset();
}

Conf	conf;

void
confinit(void)
{
	int mul;

	conf.nmach = 1;
	if(conf.nmach > MAXMACH)
		panic("confinit");
	conf.npage0 = (4*1024*1024)/BY2PG;	/* BUG */
	conf.npage1 = (4*1024*1024)/BY2PG;	/* BUG */
	conf.base0 = 0;
	conf.base1 = 32*1024*1024;
	conf.npage = conf.npage0+conf.npage1;
	conf.maxialloc = 4*1024*1024;		/* BUG */
	mul = 1;
	if(conf.npage1 > 0)
		mul = 2;
	conf.nproc = 50*mul;
	conf.npgrp = 12*mul;
	conf.nseg = conf.nproc*4;
	conf.npagetab = conf.nseg*2;
	conf.nswap = 4096;
	conf.nimage = 50;
	conf.nalarm = 1000;
	conf.nchan = 200*mul;
	conf.nenv = 100*mul;
	conf.nenvchar = 8000*mul;
	conf.npgenv = 200*mul;
	conf.nmtab = 50*mul;
	conf.nmount = 80*mul;
	conf.nmntdev = 10*mul;
	conf.nmntbuf = conf.nmntdev+3;
	conf.nmnthdr = 2*conf.nmntdev;
	conf.nstream = 40 + 32*mul;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 24 * conf.nstream;
	conf.nsrv = 16*mul;			/* was 32 */
	conf.nbitmap = 300*mul;
	conf.nbitbyte = 300*1024*mul;
	conf.nfont = 10*mul;
	conf.nnoifc = 1;
	conf.nnoconv = 32;
	conf.nurp = 32;
	conf.nasync = 1;
	conf.npipe = conf.nstream/2;
	conf.nservice = 3*mul;			/* was conf.nproc/5 */
	conf.nfsyschan = 31 + conf.nchan/20;
	conf.copymode = 0;		/* copy on write */
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;
	conf.cntrlp = 0;
}

/*
 *  set up the lance
 */
void
lancesetup(Lance *lp)
{
	KMap *k;
	ushort *sp;
	uchar *cp;
	ulong pa, pte, va;
	int i, j;

	k = kmappa(ETHER, PTEIO);
	lp->rdp = (void*)(k->va+0);
	lp->rap = (void*)(k->va+2);
	k = kmappa(EEPROM, PTEIO);
	cp = (uchar*)(k->va+0x7da);
	for(i=0; i<6; i++)
		lp->ea[i] = *cp++;
	kunmap(k);

	lp->lognrrb = 7;
	lp->logntrb = 5;
	lp->nrrb = 1<<lp->lognrrb;
	lp->ntrb = 1<<lp->logntrb;
	lp->sep = 1;
	lp->busctl = BSWP | ACON | BCON;

	/*
	 * Allocate area for lance init block and descriptor rings
	 */
	pa = (ulong)ialloc(BY2PG, 1)&~KZERO;	/* one whole page */
	/* map at LANCESEGM */
	k = kmappa(pa, PTEMAINMEM);
print("init block va %lux\n", k->va);
	lp->lanceram = (ushort*)k->va;
	lp->lm = (Lancemem*)k->va;

	/*
	 * Allocate space in host memory for the io buffers.
	 * Allocate a block and kmap it page by page.  kmap's are initially
	 * in reverse order so rearrange them.
	 */
	i = (lp->nrrb+lp->ntrb)*sizeof(Etherpkt);
	i = (i+(BY2PG-1))/BY2PG;
print("%d lance buffers\n", i);
	pa = (ulong)ialloc(i*BY2PG, 1)&~KZERO;
	va = 0;
	for(j=i-1; j>=0; j--){
		k = kmappa(pa+j*BY2PG, PTEMAINMEM);
		if(va){
			if(va != k->va+BY2PG)
				panic("lancesetup va unordered");
			va = k->va;
		}
	}
	/*
	 * k->va is the base of the region
	 */
	lp->lrp = (Etherpkt*)k->va;
	lp->rp = (Etherpkt*)k->va;
	lp->ltp = lp->lrp+lp->nrrb;
	lp->tp = lp->rp+lp->nrrb;
}

void
firmware(void)
{
	systemreset();
}
