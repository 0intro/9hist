#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

typedef struct Boot{
	long station;
	long traffic;
	char user[NAMELEN];
	char server[64];
	char line[64];
}Boot;
#define BOOT ((Boot*)0)

char protouser[NAMELEN];

void unloadboot(void);

void
main(void)
{
	unloadboot();
	machinit();
	mmuinit();
	confinit();
	printinit();
	flushmmu();
	procinit0();
	pgrpinit();
	chaninit();
	alarminit();
	chandevreset();
	streaminit();
	pageinit();
	userinit();
	schedinit();
}

void
unloadboot(void)
{
	strncpy(protouser, BOOT->user, NAMELEN);
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
	m->proc = u->p;
	u->p->state = Running;
	u->p->mach = m;
	spllo();
	chandevinit();
	
	u->slash = (*devtab[0].attach)(0);
	u->dot = clone(u->slash, 0);

	touser();
}

FPsave	initfp;

void
userinit(void)
{
	Proc *p;
	Seg *s;
	User *up;

	p = newproc();
	p->pgrp = newpgrp();
	strcpy(p->text, "*init*");
	strcpy(p->pgrp->user, protouser);
/*	savefpregs(&initfp);	/**/
/*	p->fpstate = FPinit;	/**/

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
	up = (User*)(p->upage->pa|KZERO);
	up->p = p;

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
	memcpy((ulong*)(s->o->pte[0].page->pa|KZERO), initcode, sizeof initcode);
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

Conf	conf;

void
confinit(void)
{
	conf.nmach = 1;
	if(conf.nmach > MAXMACH)
		panic("confinit");
	conf.nproc = 32;
	conf.npgrp = 15;
	conf.npage = (4*1024*1024-256*1024)/BY2PG;
	conf.npte = 500;
	conf.nmod = 50;
	conf.nalarm = 1000;
	conf.norig = 50;
	conf.nchan = 200;
	conf.nenv = 100;
	conf.nenvchar = 8000;
	conf.npgenv = 200;
	conf.nmtab = 50;
	conf.nmount = 100;
	conf.nmntdev = 5;
	conf.nmntbuf = 10;
	conf.nmnthdr = 10;
	conf.nstream = 64;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 12 * conf.nstream;
	conf.nsrv = 32;
}
