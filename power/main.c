#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"

/*
 * software tlb simulation
 */
Softtlb stlb[4][STLBSIZE];

/*
 *  args passed by boot process
 */
int _argc; char **_argv; char **_env;

/*
 *  arguments passed to initcode
 */
char argbuf[512];
int argsize;

/*
 *  environment passed to any kernel started by this kernel
 */
char envbuf[64];
char *env[2];

/*
 *  configuration file read by boot program
 */
char confbuf[4*1024];

/*
 *  system name
 */
char sysname[64];

/*
 *  IO board type
 */
int ioid;

void
main(void)
{
	machinit();
	active.exiting = 0;
	active.machs = 1;
	confinit();
	arginit();
	lockinit();
	printinit();
	duartspecial(0, &printq, &kbdq, 9600);
	tlbinit();
	vecinit();
	procinit0();
	initseg();
	grpinit();
	chaninit();
	clockinit();
	alarminit();
	ioboardinit();
	chandevreset();
	streaminit();
	swapinit();
	pageinit();
	userinit();
	launchinit();
	schedinit();
}

void
machinit(void)
{
	int n;

	icflush(0, 64*1024);
	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->stb = &stlb[n][0];
	duartinit();
}

void
tlbinit(void)
{
	int i;

	for(i=0; i<NTLB; i++)
		puttlbx(i, KZERO | PTEPID(i), 0);
}

void
vecinit(void)
{
	ulong *p, *q;
	int size;

	p = (ulong*)EXCEPTION;
	q = (ulong*)vector80;
	for(size=0; size<4; size++)
		*p++ = *q++;
	p = (ulong*)UTLBMISS;
	q = (ulong*)vector0;
	for(size=0; size<0x80/sizeof(*q); size++)
		*p++ = *q++;
}

/*
 *  reset the vme bus
 */
void
vmereset(void)
{
	long i;
	int noforce;

	if(ioid >= IO3R1)
		noforce = 1;
	else
		noforce = 0;
	MODEREG->resetforce = (1<<1) | noforce;
	delay(140);
	MODEREG->resetforce = noforce;
}

/*
 *  We have to program both the IO2 board to generate interrupts
 *  and the SBCC on CPU 0 to accept them.
 */
void
ioboardinit(void)
{
	long i;
	int maxlevel;
	ushort *sp;

	ioid = *IOID;
	if(ioid >= IO3R1)
		maxlevel = 8;
	else
		maxlevel = 8;

	vmereset();
	MODEREG->masterslave = (SLAVE<<4) | MASTER;

	/*
	 *  all VME interrupts to the error routine
	 */
	for(i=0; i<256; i++)
		setvmevec(i, novme);

	/*
	 *  tell IO2 to sent all interrupts to CPU 0's SBCC
	 */
	for(i=0; i<maxlevel; i++)
		INTVECREG->i[i].vec = 0<<8;

	/*
	 *  Tell CPU 0's SBCC to map all interrupts from the IO2 to MIPS level 5
	 *
	 *	0x01		level 0
	 *	0x02		level 1
	 *	0x04		level 2
	 *	0x08		level 4
	 *	0x10		level 5
	 */
	SBCCREG->flevel = 0x10;

	/*
	 *  Tell CPU 0's SBCC to enable all interrupts from the IO2.
	 *
	 *  The SBCC 16 bit registers are read/written as ulong, but only
	 *  bits 23-16 and 7-0 are meaningful.
	 */
	SBCCREG->fintenable |= 0xff;	  /* allow all interrupts on the IO2 */
	SBCCREG->idintenable |= 0x800000; /* allow interrupts from the IO2 */

	/*
	 *  Enable all interrupts on the IO2.  If IO3, run in compatibility mode.
	 */
	*IO2SETMASK = 0xff000000;

}

void
launchinit(void)
{
	int i;

	for(i=1; i<conf.nmach; i++)
		launch(i);
	for(i=0; i<1000000; i++)
		if(active.machs == (1<<conf.nmach) - 1){
			print("all launched\n");
			return;
		}
	print("launch: active = %x\n", active.machs);
}


void
init0(void)
{
	int i;
	ulong *sp;
	Chan *c;

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
		ksetenv("cputype", "mips");
		ksetterm("sgi %s 4D");
		ksetenv("sysname", sysname);
		poperror();
	}

	sp = (ulong*)(USTKTOP - argsize);
	touser(sp);
}

FPsave	initfp;

void
userinit(void)
{
	Proc *p;
	Segment *s;
	User *up;
	KMap *k;
	int i;
	char **av;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = newegrp();
	p->fgrp = newfgrp();
	p->procmode = 0640;

	strcpy(p->text, "*init*");
	strcpy(p->user, eve);
	savefpregs(&initfp);
	p->fpstate = FPinit;

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (ulong)init0;
	p->sched.sp = USERADDR+BY2PG-(1+MAXSYSARG)*BY2WD;
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));

	/*
	 * User
	 */
	k = kmap(p->upage);
	up = (User*)VA(k);
	up->p = p;
	up->fpsave.fpstatus = initfp.fpstatus;
	kunmap(k);

	/*
	 * User Stack, pass input arguments to boot process
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKSIZE/BY2PG);
	p->seg[SSEG] = s;
	pg = newpage(1, 0, USTKTOP-BY2PG);
	segpage(s, pg);

	memmove((ulong*)(pg->pa|KZERO|(BY2PG-argsize)), 
		argbuf + sizeof(argbuf) - argsize, argsize);

	av = (char **)(pg->pa|KZERO|(BY2PG-argsize));
	for(i = 0; i < _argc; i++)
		av[i] += (char *)USTKTOP - (argbuf + sizeof(argbuf));

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
lights(int v)
{

	*LED = ~v;
}

typedef struct Beef	Beef;
struct	Beef
{
	long	deadbeef;
	long	sum;
	long	cpuid;
	long	virid;
	long	erno;
	void	(*launch)(void);
	void	(*rend)(void);
	long	junk1[4];
	long	isize;
	long	dsize;
	long	nonbss;
	long	junk2[18];
};

void
launch(int n)
{
	Beef *p;
	long i, s;
	ulong *ptr;

	p = (Beef*) 0xb0000500 + n;
	p->launch = newstart;
	p->sum = 0;
	s = 0;
	ptr = (ulong*)p;
	for (i = 0; i < sizeof(Beef)/sizeof(ulong); i++)
		s += *ptr++;
	p->sum = -(s+1);

	for(i=0; i<3000000; i++)
		if(p->launch == 0)
			break;
}

void
online(void)
{

	machinit();
	lock(&active);
	active.machs |= 1<<m->machno;
	unlock(&active);
	tlbinit();
	clockinit();
	schedinit();
}

void
exit(void)
{
	int i;

	u = 0;
	lock(&active);
	active.machs &= ~(1<<m->machno);
	active.exiting = 1;
	unlock(&active);
	spllo();
	print("cpu %d exiting\n", m->machno);
	while(active.machs || consactive())
		for(i=0; i<1000; i++)
			;
	splhi();
	for(i=0; i<2000000; i++)
		;
	duartenable0();
	firmware();
}

typedef struct Conftab {
	char *sym;
	ulong *x;
} Conftab;

#include "conf.h"

Conf	conf;

ulong
confeval(char *exp)
{
	char *op;
	Conftab *ct;

	/* crunch leading white */
	while(*exp==' ' || *exp=='\t')
		exp++;

	op = strchr(exp, '+');
	if(op != 0){
		*op++ = 0;
		return confeval(exp) + confeval(op);
	}

	op = strchr(exp, '*');
	if(op != 0){
		*op++ = 0;
		return confeval(exp) * confeval(op);
	}

	if(*exp >= '0' && *exp <= '9')
		return strtoul(exp, 0, 0);

	/* crunch trailing white */
	op = strchr(exp, ' ');
	if(op)
		*op = 0;
	op = strchr(exp, '\t');
	if(op)
		*op = 0;

	/* lookup in symbol table */
	for(ct = conftab; ct->sym; ct++)
		if(strcmp(exp, ct->sym) == 0)
			return *(ct->x);

	return 0;
}

/*
 *  each line of the configuration is of the form `param = expression'.
 */
void
confset(char *sym)
{
	char *val, *p;
	Conftab *ct;
	ulong x;

	/*
 	 *  parse line
	 */

	/* comment */
	if(p = strchr(sym, '#'))
		*p = 0;

	/* skip white */
	for(p = sym; *p==' ' || *p=='\t'; p++)
		;
	sym = p;

	/* skip sym */
	for(; *p && *p!=' ' && *p!='\t' && *p!='='; p++)
		;
	if(*p)
		*p++ = 0;

	/* skip white */
	for(; *p==' ' || *p=='\t' || *p=='='; p++)
		;
	val = p;

	/*
	 *  lookup value
	 */
	for(ct = conftab; ct->sym; ct++)
		if(strcmp(sym, ct->sym) == 0){
			*(ct->x) = confeval(val);
			return;
		}

	if(strcmp(sym, "sysname")==0){
		p = strchr(val, ' ');
		if(p)
			*p = 0;
		strcpy(sysname, val);
	} else if(strcmp(sym, "eve")==0){
		p = strchr(val, ' ');
		if(p)
			*p = 0;
		strcpy(eve, val);
	}
}

/*
 *  read the ascii configuration left by the boot kernel
 */
void
confread(void)
{
	char *line;
	char *end;

	/*
	 *  process configuration file
	 */
	line = confbuf;
	while(end = strchr(line, '\n')){
		*end = 0;
		confset(line);
		line = end+1;
	}
}

void
confprint(void)
{
	Conftab *ct;

	/*
	 *  lookup value
	 */
	for(ct = conftab; ct->sym; ct++)
		print("%s == %d\n", ct->sym, *ct->x);
}

void
confinit(void)
{
	long x, i, j, *l;

	/*
	 *  copy configuration down from high memory
	 */
	strcpy(confbuf, (char *)(0x80000000 + 4*1024*1024 - 4*1024));

	/*
	 *  size memory
	 */
	x = 0x12345678;
	for(i=4; i<128; i+=4){
		l = (long*)(KSEG1|(i*1024L*1024L));
		*l = x;
		wbflush();
		*(ulong*)KSEG1 = *(ulong*)KSEG1;	/* clear latches */
		if(*l != x)
			break;
		x += 0x3141526;
	}
	conf.npage0 = i*1024/4;
	conf.base0 = 0;
	conf.npage = conf.npage0;
	conf.npage1 = 0;
	conf.base1 = 0;
	conf.maxialloc = 16*1024*1024;

	/*
 	 *  clear MP bus error caused by sizing memory
	 */
	i = *SBEADDR;
	USED(i);

	/*
	 *  set minimal default values
	 */
	conf.nmach = 1;
	conf.nproc = 100;
	conf.npgrp = conf.nproc / 4;
	conf.npgenv = 4 * conf.npgrp;
	conf.nenv = 4 * conf.nproc;
	conf.nenvchar = 20 * conf.nenv;
	conf.nmtab = conf.nproc;
	conf.nseg = 4 * conf.nproc;
	conf.npagetab = conf.nseg*3;
	conf.nswap = 262144;
	conf.nimage = 200;
	conf.nchan = 20 * conf.nproc;
	conf.nmntdev = conf.nproc;
	conf.nmntbuf = conf.nproc;
	conf.nmnthdr = conf.nproc;
	conf.nstream = 2 * conf.nproc;
	conf.nalarm = 500;
	conf.nmount = 500;
	conf.nsrv = 20;
	conf.nurp = 25;
	conf.dkif = 1;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 10 * conf.nstream;
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;

	confread();

	conf.npipe = conf.nstream/2;	/* must be after confread */
	if(conf.nmach > MAXMACH)
		panic("confinit");

	conf.copymode = 1;		/* copy on reference */
	conf.cntrlp = 1;
}

/*
 *  copy arguments passed by the boot kernel (or ROM) into a temporary buffer.
 *  we do this because the arguments are in memory that may be allocated
 *  to processes or kernel buffers.
 */
#define SYSENV "netaddr="
void
arginit(void)
{
	int i, n;
	int nbytes;
	int ssize;
	char *p;
	char **argv;
	char *charp;

	/*
	 *  get the system name from the environment
	 */
	if(*sysname == 0){
		for(argv = _env; *argv; argv++){
			if(strncmp(*argv, SYSENV, sizeof(SYSENV)-1)==0){
				strcpy(sysname, (*argv) + sizeof(SYSENV)-1);
				break;
			}
		}
	}
	strcpy(envbuf, SYSENV);
	strcat(envbuf, sysname);
	env[0] = envbuf;
	env[1] = 0;

	/*
	 *  trim arguments to make them fit in the buffer (argv[0] is sysname)
	 */
	nbytes = 0;
	_argv[0] = sysname;
	for(i = 0; i < _argc; i++){
		n = strlen(_argv[i]) + 1;
		ssize = BY2WD*(i+2) + ((nbytes+n+(BY2WD-1)) & ~(BY2WD-1));
		if(ssize > sizeof(argbuf))
			break;
		nbytes += n;
	}
	_argc = i;
	ssize = BY2WD*(i+1) + ((nbytes+(BY2WD-1)) & ~(BY2WD-1));

	/*
	 *  copy arguments into the buffer
	 */
	argv = (char**)(argbuf + sizeof(argbuf) - ssize);
	charp = (char*)(argbuf + sizeof(argbuf) - nbytes);
	for(i=0; i<_argc; i++){
		argv[i] = charp;
		n = strlen(_argv[i]) + 1;
		memmove(charp, _argv[i], n);
		charp += n;
	}
	_argv = argv;
	argsize = ssize;
}

/*
 *  setup the IO2 lance, io buffers are in lance memory
 */
void
lanceIO2setup(Lance *lp)
{
	ushort *sp;

	/*
	 *  reset lance and set parity on its memory
	 */
	MODEREG->promenet &= ~1;
	MODEREG->promenet |= 1;
	for(sp = LANCERAM; sp < LANCEEND; sp += 1)
		*sp = 0;

	lp->sep = 1;
	lp->lanceram = LANCERAM;
	lp->lm = (Lancemem*)0;

	/*
	 *  Allocate space in lance memory for the io buffers.
	 *  Start at 4k to avoid the initialization block and
	 *  descriptor rings.
	 */
	lp->lrp = (Etherpkt*)(4*1024);
	lp->ltp = lp->lrp + lp->nrrb;
	lp->rp = (Etherpkt*)(((ulong)LANCERAM) + (ulong)lp->lrp);
	lp->tp = lp->rp + lp->nrrb;
}

/*
 *  setup the IO3 lance, io buffers are in host memory mapped to
 *  lance address space
 */
void
lanceIO3setup(Lance *lp)
{
	ulong x, y;
	int index;
	ushort *sp;
	int len;

	/*
	 *  reset lance and set parity on its memory
	 */
	MODEREG->promenet |= 1;
	MODEREG->promenet &= ~1;
	for(sp = LANCE3RAM; sp < LANCE3END; sp += 2)
		*sp = 0;

	lp->sep = 4;
	lp->lanceram = LANCE3RAM;
	lp->lm = (Lancemem*)0x800000;

	/*
	 *  allocate some host memory for buffers and map it into lance
	 *  space
	 */
	len = (lp->nrrb + lp->ntrb)*sizeof(Etherpkt);
	lp->rp = (Etherpkt*)ialloc(len , 1);
	lp->tp = lp->rp + lp->nrrb;
	x = (ulong)lp->rp;
	lp->lrp = (Etherpkt*)(x & 0xFFF);
	lp->ltp = lp->lrp + lp->nrrb;
	index = LANCEINDEX;
	for(y = x+len; x < y; x += 0x1000){
		*WRITEMAP = (index<<16) | (x>>12)&0xFFFF;
		index++;
	}
}

/*
 *  set up the lance
 */
void
lancesetup(Lance *lp)
{
	lp->rap = LANCERAP;
	lp->rdp = LANCERDP;
	lp->ea[0] = LANCEID[20]>>8;
	lp->ea[1] = LANCEID[16]>>8;
	lp->ea[2] = LANCEID[12]>>8;
	lp->ea[3] = LANCEID[8]>>8;
	lp->ea[4] = LANCEID[4]>>8;
	lp->ea[5] = LANCEID[0]>>8;
	lp->lognrrb = 7;
	lp->logntrb = 7;
	lp->nrrb = 1<<lp->lognrrb;
	lp->ntrb = 1<<lp->logntrb;
	lp->busctl = BSWP;
	if(ioid >= IO3R1)
		lanceIO3setup(lp);
	else
		lanceIO2setup(lp);
}

void
lanceparity(void)
{
	print("lance DRAM parity error\n");
	MODEREG->promenet &= ~4;
	MODEREG->promenet |= 4;
}

/*
 *  for the sake of a single devcons.c
 */
void
buzz(int f, int d)
{
	USED(f);
}

int
mouseputc(IOQ *q, int c)
{
	USED(q);
	return 0;
}

