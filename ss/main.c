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
	printinit();
	print("sparc plan 9\n");
	trapinit();
	kmapinit();
	cacheinit();
	intrinit();
	procinit0();
	pgrpinit();
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
reset(void)
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

	touser(USTKTOP-5*BY2WD);
}

FPsave	initfp;

void
userinit(void)
{
	Proc *p;
	Seg *s;
	User *up;
	KMap *k;

	p = newproc();
	p->pgrp = newpgrp();
	strcpy(p->text, "*init*");
	savefpregs(&initfp);
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (((ulong)init0) - 8);	/* 8 because of RETURN in gotolabel */
	p->sched.sp = USERADDR+BY2PG-20;	/* BUG */
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
	s = &p->seg[SSEG];
	s->proc = p;
	s->o = neworig(USTKTOP-BY2PG, 1, OWRPERM, 0);
	s->minva = USTKTOP-BY2PG;
	s->maxva = USTKTOP;

	/*
	 * Text
	 */
	s = &p->seg[TSEG];
	s->proc = p;
	s->o = neworig(UTZERO, 1, 0, 0);
	s->o->pte[0].page = newpage(0, 0, UTZERO);
	s->o->npage = 1;
	k = kmap(s->o->pte[0].page);
	memmove((ulong*)VA(k), initcode, sizeof initcode);
	kunmap(k);
	s->minva = UTZERO;
	s->maxva = UTZERO+BY2PG;

	ready(p);
}

void
exit(void)
{
	int i;

	u = 0;
	splhi();
	print("exiting\n");
	delay(30*1000);
	reset();
}

/*
 * Insert new into list after where
 */
void
insert(List **head, List *where, List *new)
{
	if(where == 0){
		new->next = *head;
		*head = new;
	}else{
		new->next = where->next;
		where->next = new;
	}
		
}

/*
 * Insert new into list at end
 */
void
append(List **head, List *new)
{
	List *where;

	where = *head;
	if(where == 0)
		*head = new;
	else{
		while(where->next)
			where = where->next;
		where->next = new;
	}
	new->next = 0;
}

/*
 * Delete old from list
 */
void
delete0(List **head, List *old)
{
	List *l;

	l = *head;
	if(l == old){
		*head = old->next;
		return;
	}
	while(l->next != old)
		l = l->next;
	l->next = old->next;
}

/*
 * Delete old from list.  where->next is known to be old.
 */
void
delete(List **head, List *where, List *old)
{
	if(where == 0){
		*head = old->next;
		return;
	}
	where->next = old->next;
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
	conf.npte = 1400*mul;
	conf.nmod = 800*mul;
	conf.nalarm = 1000;
	conf.norig = 150*mul;
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

	lp->lognrrb = 5;
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
