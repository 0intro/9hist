#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"

#include	<libg.h>
#include	<gnot.h>

uchar	*intrreg;
int	model;
uchar	idprom[32];
ulong	romvec;		/* open boot rom vector */
int	cpuserver;
ulong	romputcxsegm;
ulong	bank[2];

void
main(void)
{
	u = 0;
	memset(edata, 0, (char*)end-(char*)edata);

	machinit();
	active.exiting = 0;
	active.machs = 1;
	confinit();
	xinit();
	mmuinit();
	if(conf.monitor)
		screeninit();
	printinit();
	trapinit();
	kmapinit();
	ioinit();
	if(!conf.monitor)
		sccspecial(2, &printq, &kbdq, 9600);
	pageinit();
	cacheinit();
	intrinit();
	procinit0();
	initseg();
	chandevreset();
	streaminit();
	swapinit();
	userinit();
	clockinit();
	schedinit();
}

void
intrinit(void)
{
	KMap *k;

	k = kmappa(INTRREG, PTEIO|PTENOCACHE);
	intrreg = (uchar*)k->va;
}

void
systemreset(void)
{
	delay(100);
	putenab(ENABRESET);
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

	/* tell scc driver its addresses */
	k = kmappa(KMDUART, PTEIO|PTENOCACHE);
	sccsetup((void*)(k->va), KMFREQ);
	k = kmappa(EIADUART, PTEIO|PTENOCACHE);
	sccsetup((void*)(k->va), EIAFREQ);

	/* scc port 0 is the keyboard */
	sccspecial(0, 0, &kbdq, 2400);
	kbdq.putc = kbdstate;

	/* scc port 1 is the mouse */
	sccspecial(1, 0, &mouseq, 2400);
}

void
init0(void)
{
	u->nerrlab = 0;
	m->proc = u->p;
	u->p->state = Running;
	u->p->mach = m;
	spllo();

	print("bank 0: %dM  1: %dM\n", bank[0], bank[1]);
	u->slash = (*devtab[0].attach)(0);
	u->dot = clone(u->slash, 0);

	kproc("alarm", alarmkproc, 0);
	chandevinit();

	if(!waserror()){
		ksetterm("sun %s");
		ksetenv("cputype", "sparc");
		poperror();
	}

	touser(USTKTOP-(1+MAXSYSARG)*BY2WD);
}

FPsave	*initfpp;
uchar	initfpa[sizeof(FPsave)+7];

void
userinit(void)
{
	Proc *p;
	Segment *s;
	User *up;
	KMap *k;
	ulong l;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = smalloc(sizeof(Fgrp));
	p->fgrp->ref = 1;
	p->procmode = 0640;

	strcpy(p->text, "*init*");
	strcpy(p->user, eve);
	/* must align initfpp to an ODD word boundary */
	l = (ulong)initfpa;
	l += 3;
	l &= ~7;
	l += 4;
	initfpp = (FPsave*)l;
	savefpregs(initfpp);
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (((ulong)init0) - 8);	/* 8 because of RETURN in gotolabel */
	p->sched.sp = USERADDR+BY2PG-(1+MAXSYSARG)*BY2WD;
	p->sched.sp &= ~7;		/* SP must be 8-byte aligned */
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
	print("cpu %d exiting\n", m->machno);
	while(consactive())
		for(i=0; i<1000; i++)
			;
	splhi();
	systemreset();
}

int
banksize(ulong addr, ulong nbytes)
{
	int i;
	ulong min, max, t;
	ulong va, pa;
	ulong nmeg;

	nmeg = nbytes/MB;
	va = 1*MB-2*BY2PG;
	for(i=0; i<nmeg; i++){
		pa = addr+i*MB;
		putw4(va, PPN(pa)|PTEVALID|PTEKERNEL|PTENOCACHE|PTEWRITE|PTEMAINMEM);
		*(ulong*)va = pa;
	}
	min = ~0;
	max = 0;
	for(i=0; i<nmeg; i++){
		pa = addr+i*MB;
		putw4(va, PPN(pa)|PTEVALID|PTEKERNEL|PTENOCACHE|PTEWRITE|PTEMAINMEM);
		t = *(ulong*)va;
		if(min > t)
			min = t;
		if(max < t)
			max = t;
	}
	putw4(va, INVALIDPTE);
	return (max-min)/MB+1;
}

Conf	conf;

void
confinit(void)
{
	int mul;
	ulong i;
	ulong ktop, va, mbytes;

	conf.nmach = 1;
	if(conf.nmach > MAXMACH)
		panic("confinit");

	/* map id prom */
	va = 1*MB-BY2PG;
	putw4(va, PPN(EEPROM)|PTEVALID|PTEKERNEL|PTENOCACHE|PTEIO);
	memmove(idprom, (char*)(va+0x7d8), 32);
	if(idprom[0]!=1 || (idprom[1]&0xF0)!=0x50)
		*(ulong*)va = 0;
	putw4(va, INVALIDPTE);

	switch(idprom[1]){
	case 0x51:		/* sparcstation 1 */
	case 0x54:		/* slc */
	default:
		conf.ss2 = 0;
		conf.vacsize = 65536;
		conf.vaclinesize = 16;
		conf.ncontext = 8;
		conf.npmeg = 128;
		conf.ss2cachebug = 0;
		conf.monitor = 1;		/* BUG */
		conf.base0 = 0;
		conf.base1 = 32*MB;
		break;

	case 0x55:		/* sparcstation 2 */
		conf.ss2 = 1;
		conf.vacsize = 65536;
		conf.vaclinesize = 32;
		conf.ncontext = 16;
		conf.npmeg = 256;
		conf.ss2cachebug = 1;
		conf.monitor = 0;		/* BUG */
		conf.base0 = 0;
		conf.base1 = 16*MB;
		break;
	}

	bank[0] = banksize(conf.base0, 16*MB);
	bank[1] = banksize(conf.base1, 16*MB);
	conf.npage0 = (bank[0]*MB)/BY2PG;
	conf.npage1 = (bank[1]*MB)/BY2PG;

	romputcxsegm = *(ulong*)(romvec+260);

	conf.npage = conf.npage0+conf.npage1;
	conf.upages = (conf.npage*70)/100;
	if(cpuserver){
		i = conf.npage-conf.upages;
		if(i > (6*MB)/BY2PG)
			conf.upages +=  i - ((6*MB)/BY2PG);
	}

	ktop = PGROUND((ulong)end);
	ktop = PADDR(ktop);
	conf.npage0 -= ktop/BY2PG;
	conf.base0 += ktop;

	mbytes = (conf.npage*BY2PG)>>20;
	mul = 1 + (mbytes+11)/12;
	if(mul > 2)
		mul = 2;

	conf.nproc = 50*mul;
	if(cpuserver)
		conf.nswap = conf.npage*2;
	else
		conf.nswap = 4096;
	conf.nimage = 50;
	conf.copymode = 0;		/* copy on write */
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;
	if(cpuserver)
		conf.nproc = 500;
}

/*
 *  set up the lance
 */
void
lancesetup(Lance *lp)
{
	KMap *k;
	uchar *cp;
	ulong pa, va;
	int i, j;

	k = kmappa(ETHER, PTEIO|PTENOCACHE);
	lp->rdp = (void*)(k->va+0);
	lp->rap = (void*)(k->va+2);
	k = kmappa(EEPROM, PTEIO|PTENOCACHE);
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
	pa = (ulong)xspanalloc(BY2PG, BY2PG, 0);

	/* map at LANCESEGM */
	k = kmappa(pa, PTEMAINMEM|PTENOCACHE);
	lp->lanceram = (ushort*)k->va;
	lp->lm = (Lancemem*)k->va;

	/*
	 * Allocate space in host memory for the io buffers.
	 * Allocate a block and kmap it page by page.  kmap's are initially
	 * in reverse order so rearrange them.
	 */
	i = (lp->nrrb+lp->ntrb)*sizeof(Etherpkt);
	i = (i+(BY2PG-1))/BY2PG;
	pa = (ulong)xspanalloc(i*BY2PG, BY2PG, 0)&~KZERO;
	va = 0;
	for(j=i-1; j>=0; j--){
		k = kmappa(pa+j*BY2PG, PTEMAINMEM|PTENOCACHE);
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
