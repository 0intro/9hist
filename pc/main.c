#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"

Mach *m;

static  uchar *sp;	/* stack pointer for /boot */

/*
 * Where configuration info is left for the loaded programme.
 * This will turn into a structure as more is done by the boot loader
 * (e.g. why parse the .ini file twice?).
 * There are 1024 bytes available at CONFADDR.
 */
#define BOOTLINE	((char*)CONFADDR)
#define BOOTLINELEN	64
#define BOOTARGS	((char*)(CONFADDR+BOOTLINELEN))
#define	BOOTARGSLEN	(1024-BOOTLINELEN)
#define	MAXCONF		32

char bootdisk[NAMELEN];
char *confname[MAXCONF];
char *confval[MAXCONF];
int nconf;

extern void ns16552install(void);	/* botch: config */

static int isoldbcom;

static int
getcfields(char* lp, char** fields, int n, char* sep)
{
	int i;

	for(i = 0; lp && *lp && i < n; i++){
		while(*lp && strchr(sep, *lp) != 0)
			*lp++ = 0;
		if(*lp == 0)
			break;
		fields[i] = lp;
		while(*lp && strchr(sep, *lp) == 0){
			if(*lp == '\\' && *(lp+1) == '\n')
				*lp++ = ' ';
			lp++;
		}
	}

	return i;
}

static void
options(void)
{
	uchar *bda;
	long i, n;
	char *cp, *line[MAXCONF], *p, *q;

	if(strncmp(BOOTARGS, "ZORT 0\r\n", 8)){
		isoldbcom = 1;

		memmove(BOOTARGS, KADDR(1024), BOOTARGSLEN);
		memmove(BOOTLINE, KADDR(0x100), BOOTLINELEN);

		bda = KADDR(0x400);
		bda[0x13] = 639;
		bda[0x14] = 639>>8;
	}

	/*
	 *  parse configuration args from dos file plan9.ini
	 */
	cp = BOOTARGS;	/* where b.com leaves its config */
	cp[BOOTARGSLEN-1] = 0;

	/*
	 * Strip out '\r', change '\t' -> ' '.
	 */
	p = cp;
	for(q = cp; *q; q++){
		if(*q == '\r')
			continue;
		if(*q == '\t')
			*q = ' ';
		*p++ = *q;
	}
	*p = 0;

	n = getcfields(cp, line, MAXCONF, "\n");
	for(i = 0; i < n; i++){
		if(*line[i] == '#')
			continue;
		cp = strchr(line[i], '=');
		if(cp == 0)
			continue;
		*cp++ = 0;
		if(cp - line[i] >= NAMELEN+1)
			*(line[i]+NAMELEN-1) = 0;
		confname[nconf] = line[i];
		confval[nconf] = cp;
		nconf++;
	}
}

void
main(void)
{
	outb(0x3F2, 0x00);			/* botch: turn off the floppy motor */

	/*
	 * There is a little leeway here in the ordering but care must be
	 * taken with dependencies:
	 *	function		dependencies
	 *	========		============
	 *	machinit		depends on: m->machno, m->pdb
	 *	cpuidentify		depends on: m
	 *	confinit		calls: meminit
	 *	meminit			depends on: cpuidentify (needs to know processor
	 *				  type for caching, etc.)
	 *	archinit		depends on: meminit (MP config table may be at the
	 *				  top of system physical memory);
	 *				conf.nmach (not critical, mpinit will check);
	 *	arch->intrinit		depends on: trapinit
	 */
	conf.nmach = 1;
	MACHP(0) = (Mach*)CPU0MACH;
	m->pdb = (void*)CPU0PDB;
	machinit();
	active.machs = 1;
	active.exiting = 0;
	options();
	screeninit();
	cpuidentify();
	confinit();
	archinit();
	xinit();
	trapinit();
	printinit();
	cpuidprint();
	if(isoldbcom)
		print("    ****OLD B.COM - UPGRADE****\n");
	pageinit();
	mmuinit();
	if(arch->intrinit)
		arch->intrinit();
	ns16552install();			/* botch: config */
	mathinit();
	kbdinit();
	if(arch->clockenable)
		arch->clockenable();
	procinit0();
	initseg();
	links();
	chandevreset();
	swapinit();
	userinit();
	schedinit();
}

void
machinit(void)
{
	int machno;
	void *pdb;

	machno = m->machno;
	pdb = m->pdb;
	memset(m, 0, sizeof(Mach));
	m->machno = machno;
	m->pdb = pdb;
}

void
ksetterm(char *f)
{
	char buf[2*NAMELEN];

	sprint(buf, f, conffile);
	ksetenv("terminal", buf);
}

void
init0(void)
{
	int i;
	char tstr[32];

	up->nerrlab = 0;

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	up->dot = cclone(up->slash, 0);

	chandevinit();

	if(!waserror()){
		strcpy(tstr, arch->id);
		strcat(tstr, " %s");
		ksetterm(tstr);
		ksetenv("cputype", "386");
		if(cpuserver)
			ksetenv("service", "cpu");
		else
			ksetenv("service", "terminal");
		for(i = 0; i < nconf; i++)
			if(confname[i] && confname[i][0] != '*')
				ksetenv(confname[i], confval[i]);
		poperror();
	}
	kproc("alarm", alarmkproc, 0);
	touser(sp);
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	KMap *k;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = smalloc(sizeof(Fgrp));
	p->fgrp->ref = 1;
	p->fgrp->fd = smalloc(DELTAFD*sizeof(Chan*));
	p->fgrp->nfd = DELTAFD;
	p->rgrp = newrgrp();
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
	p->sched.sp = (ulong)p->kstack+KSTACK-(1+MAXSYSARG)*BY2WD;

	/*
	 * User Stack
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKSIZE/BY2PG);
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
	s->flushme++;
	p->seg[TSEG] = s;
	pg = newpage(1, 0, UTZERO);
	memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));
	segpage(s, pg);
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
	uchar **lsp;
	char *cp = BOOTLINE;
	char buf[64];

	sp = (uchar*)base + BY2PG - MAXSYSARG*BY2WD;

	ac = 0;
	av[ac++] = pusharg("/386/9dos");
	cp[BOOTLINELEN-1] = 0;
	buf[0] = 0;
	if(strncmp(cp, "fd!", 3) == 0){
		sprint(buf, "local!#f/fd%ddisk", strtol(cp+3, 0, 0));
		av[ac++] = pusharg(buf);
	} else if(strncmp(cp, "h!", 2) == 0){
		sprint(buf, "local!#H/hd%dfs", strtol(cp+2, 0, 0));
		av[ac++] = pusharg(buf);
	} else if(strncmp(cp, "hd!", 3) == 0){
		sprint(buf, "local!#H/hd%ddisk", strtol(cp+3, 0, 0));
		av[ac++] = pusharg(buf);
	} else if(strncmp(cp, "s!", 2) == 0){
		sprint(buf, "local!#w/sd%dfs", strtol(cp+2, 0, 0));
		av[ac++] = pusharg(buf);
	} else if(strncmp(cp, "sd!", 3) == 0){
		sprint(buf, "local!#w/sd%ddisk", strtol(cp+3, 0, 0));
		av[ac++] = pusharg(buf);
	} else if(strncmp(cp, "e!", 2) == 0)
		av[ac++] = pusharg("-n");
	if(buf[0]){
		cp = strchr(buf, '!');
		if(cp){
			strcpy(bootdisk, cp+1);
			addconf("bootdisk", bootdisk);
		}
	}

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
addconf(char *name, char *val)
{
	if(nconf >= MAXCONF)
		return;
	confname[nconf] = name;
	confval[nconf] = val;
	nconf++;
}

char*
getconf(char *name)
{
	int i;

	for(i = 0; i < nconf; i++)
		if(cistrcmp(confname[i], name) == 0)
			return confval[i];
	return 0;
}

void
confinit(void)
{
	char *p;
	int i, pcnt;
	ulong maxmem;
	extern int defmaxmsg;

	if(p = getconf("*maxmem"))
		maxmem = strtoul(p, 0, 0);
	else
		maxmem = 0;
	if(p = getconf("*kernelpercent"))
		pcnt = 100 - strtol(p, 0, 0);
	else
		pcnt = 0;
	if(p = getconf("*defmaxmsg")){
		i = strtol(p, 0, 0);
		if(i < defmaxmsg && i >=128)
			defmaxmsg = i;
	}

	meminit(maxmem);

	conf.npage = conf.npage0 + conf.npage1;
	if(pcnt < 10)
		pcnt = 70;
	conf.upages = (conf.npage*pcnt)/100;
	conf.ialloc = ((conf.npage-conf.upages)/2)*BY2PG;

	conf.nproc = 100 + ((conf.npage*BY2PG)/MB)*5;
	if(cpuserver)
		conf.nproc *= 3;
	if(conf.nproc > 2000)
		conf.nproc = 2000;
	conf.nswap = conf.nproc*80;
	conf.nimage = 200;
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
matherror(Ureg *ur, void*)
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

	/*
	 *  save floating point state to check out error
	 */
	fpenv(&up->fpsave);
	status = up->fpsave.status;

	msg = 0;
	for(i = 0; i < 8; i++)
		if((1<<i) & status){
			msg = mathmsg[i];
			sprint(note, "sys: fp: %s fppc=0x%lux", msg, up->fpsave.pc);
			postnote(up, 1, note, NDebug);
			break;
		}
	if(msg == 0){
		sprint(note, "sys: fp: unknown fppc=0x%lux", up->fpsave.pc);
		postnote(up, 1, note, NDebug);
	}
	if(ur->pc & KZERO)
		panic("fp: status %lux fppc=0x%lux pc=0x%lux", status,
			up->fpsave.pc, ur->pc);
}

/*
 *  math coprocessor emulation fault
 */
void
mathemu(Ureg*, void*)
{
	switch(up->fpstate){
	case FPinit:
		fpinit();
		up->fpstate = FPactive;
		break;
	case FPinactive:
		fprestore(&up->fpsave);
		up->fpstate = FPactive;
		break;
	case FPactive:
		panic("math emu", 0);
		break;
	}
}

/*
 *  math coprocessor segment overrun
 */
void
mathover(Ureg*, void*)
{
	pexit("math overrun", 0);
}

void
mathinit(void)
{
	intrenable(VectorCERR, matherror, 0, BUSUNKNOWN);
	if(X86FAMILY(m->cpuidax) == 3)
		intrenable(VectorIRQ13, matherror, 0, BUSUNKNOWN);
	intrenable(VectorCNA, mathemu, 0, BUSUNKNOWN);
	intrenable(VectorCSO, mathover, 0, BUSUNKNOWN);
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
	if(p->fpstate == FPactive){
		if(p->state == Moribund)
			fpoff();
		else
			fpsave(&up->fpsave);
		p->fpstate = FPinactive;
	}
}

void
exit(int ispanic)
{
	int ms, once;

	lock(&active);
	if(ispanic)
		active.ispanic = ispanic;
	else if(m->machno == 0 && (active.machs & (1<<m->machno)) == 0)
		active.ispanic = 0;
	once = active.machs & (1<<m->machno);
	active.machs &= ~(1<<m->machno);
	active.exiting = 1;
	unlock(&active);

	if(once)
		print("cpu%d: exiting\n", m->machno);
	spllo();
	for(ms = 5*1000; ms > 0; ms -= TK2MS(2)){
		delay(TK2MS(2));
		if(active.machs == 0 && consactive() == 0)
			break;
	}

	if(active.ispanic && m->machno == 0){
		if(cpuserver)
			delay(10000);
		else
			for(;;);
	}
	else
		delay(1000);

	arch->reset();
}

int
isaconfig(char *class, int ctlrno, ISAConf *isa)
{
	char cc[NAMELEN], *p, *q, *r;
	int n;

	sprint(cc, "%s%d", class, ctlrno);
	for(n = 0; n < nconf; n++){
		if(cistrncmp(confname[n], cc, NAMELEN))
			continue;
		isa->nopt = 0;
		p = confval[n];
		while(*p){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == '\0')
				break;
			if(cistrncmp(p, "type=", 5) == 0){
				p += 5;
				for(q = isa->type; q < &isa->type[NAMELEN-1]; q++){
					if(*p == '\0' || *p == ' ' || *p == '\t')
						break;
					*q = *p++;
				}
				*q = '\0';
			}
			else if(cistrncmp(p, "port=", 5) == 0)
				isa->port = strtoul(p+5, &p, 0);
			else if(cistrncmp(p, "irq=", 4) == 0)
				isa->irq = strtoul(p+4, &p, 0);
			else if(cistrncmp(p, "dma=", 4) == 0)
				isa->dma = strtoul(p+4, &p, 0);
			else if(cistrncmp(p, "mem=", 4) == 0)
				isa->mem = strtoul(p+4, &p, 0);
			else if(cistrncmp(p, "size=", 5) == 0)
				isa->size = strtoul(p+5, &p, 0);
			else if(cistrncmp(p, "freq=", 5) == 0)
				isa->freq = strtoul(p+5, &p, 0);
			else if(isa->nopt < NISAOPT){
				r = isa->opt[isa->nopt];
				while(*p && *p != ' ' && *p != '\t'){
					*r++ = *p++;
					if(r-isa->opt[isa->nopt] >= ISAOPTLEN-1)
						break;
				}
				*r = '\0';
				isa->nopt++;
			}
			while(*p && *p != ' ' && *p != '\t')
				p++;
		}
		return 1;
	}
	return 0;
}

int
iprint(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;
	va_list arg;

	va_start(arg, fmt);
	n = doprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	screenputs(buf, n);

	return n;
}

int
cistrcmp(char *a, char *b)
{
	int ac, bc;

	for(;;){
		ac = *a++;
		bc = *b++;
	
		if(ac >= 'A' && ac <= 'Z')
			ac = 'a' + (ac - 'A');
		if(bc >= 'A' && bc <= 'Z')
			bc = 'a' + (bc - 'A');
		ac -= bc;
		if(ac)
			return ac;
		if(bc == 0)
			break;
	}
	return 0;
}

int
cistrncmp(char *a, char *b, int n)
{
	unsigned ac, bc;

	while(n > 0){
		ac = *a++;
		bc = *b++;
		n--;

		if(ac >= 'A' && ac <= 'Z')
			ac = 'a' + (ac - 'A');
		if(bc >= 'A' && bc <= 'Z')
			bc = 'a' + (bc - 'A');

		ac -= bc;
		if(ac)
			return ac;
		if(bc == 0)
			break;
	}

	return 0;
}
