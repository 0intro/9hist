#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

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

char protouser[NAMELEN];
char bootline[64];
char bootdevice[2];
int bank[2];

void unloadboot(void);

void
main(void)
{
	unloadboot();
	machinit();
	mmuinit();
	confinit();
	kmapinit();
	printinit();
	print("bank 0: %dM  bank 1: %dM\n", bank[0], bank[1]);
	flushmmu();
	procinit0();
	pgrpinit();
	chaninit();
	alarminit();
	chandevreset();
	streaminit();
	pageinit();
	kmapinit();
	userinit();
	schedinit();
}

void
unloadboot(void)
{
	strncpy(protouser, BOOT->user, NAMELEN);
	memcpy(bootline, BOOT->line, 64);
	bootdevice[0] = BOOT->device;
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
		putmmu(l, INVALIDPTE);
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
		c = namec("#e/bootline", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootline, 64);
		close(c);
		c = namec("#e/bootdevice", Acreate, OWRITE, 0600);
		(*devtab[c->type].write)(c, bootdevice, 2);
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
	Seg *s;
	User *up;
	KMap *k;

	p = newproc();
	p->pgrp = newpgrp();
	strcpy(p->text, "*init*");
	strcpy(p->pgrp->user, protouser);
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (ulong)init0;
	p->sched.sp = USERADDR+BY2PG-20;	/* BUG */
	p->sched.sr = SUPER|SPL(7);
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
	memcpy((ulong*)VA(k), initcode, sizeof initcode);
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
	for(;;)
		;
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

banksize(int base)
{
	ulong va;

	if(&end > (int *)((KZERO|1024L*1024L)-BY2PG))
		return 0;
	va = UZERO;	/* user page 1 is free to play with */
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG);
	*(ulong*)va = 0;	/* 0 at 0M */
	putmmu(va, PTEVALID|(base+1)*1024L*1024L/BY2PG);
	*(ulong*)va = 1;	/* 1 at 1M */
	putmmu(va, PTEVALID|(base+4)*1024L*1024L/BY2PG);
	*(ulong*)va = 4;	/* 4 at 4M */
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG);
	if(*(ulong*)va == 0)
		return 16;
	putmmu(va, PTEVALID|(base+1)*1024L*1024L/BY2PG);
	if(*(ulong*)va == 1)
		return 4;
	putmmu(va, PTEVALID|(base+0)*1024L*1024L/BY2PG);
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
	mul = 1 + (conf.npage1>0);
	conf.nproc = 50*mul;
	conf.npgrp = 12*mul;
	conf.npte = 700*mul;
	conf.nmod = 400*mul;
	conf.nalarm = 1000;
	conf.norig = 150*mul;
	conf.nchan = 200*mul;
	conf.nenv = 100*mul;
	conf.nenvchar = 8000*mul;
	conf.npgenv = 200*mul;
	conf.nmtab = 50*mul;
	conf.nmount = 80*mul;
	conf.nmntdev = 10*mul;
	conf.nmntbuf = 2*conf.nmntdev;
	conf.nmnthdr = 2*conf.nmntdev;
	conf.nstream = 64;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 32 * conf.nstream;
	conf.nsrv = 32*mul;
	conf.nbitmap = 300*mul;
	conf.nbitbyte = 300*1024*mul;
	if(*(uchar*)MOUSE & (1<<4))
		conf.nbitbyte *= 2;	/* ldepth 1 */
	conf.nfont = 10*mul;
	conf.nurp = 32;
	conf.nasync = 1;
}
