#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"
#include	"pool.h"

#define MAXCONF 32

char *confname[MAXCONF];
char *confval[MAXCONF];
int nconf;

int	predawn = 1;

typedef struct Bootargs Bootargs;

struct Bootargs
{
	char	args[1000];
	uchar	chksum;
};

Conf	conf;

static void options(void);

void
main(void)
{
	machinit();
	clockinit();
	confinit();
	xinit();
	trapinit();
	printinit();
	cpminit();
	uartinstall();
	mmuinit();

// turn on pcmcia
*(uchar*)(NVRAMMEM+0x100001) |= 0x60;

	pageinit();
	procinit0();
	initseg();
	options();
	links();
	chandevreset();
	swapinit();
	userinit();
predawn = 0;
	schedinit();
}

void
machinit(void)
{
	IMM *io;
	int mf, osc;

	memset(m, 0, sizeof(*m));
	m->delayloop = 20000;
	m->cputype = getpvr()>>16;
	m->iomem = KADDR(INTMEM);

	io = m->iomem;
	osc = 50;
	mf = io->plprcr >> 20;
	m->oscclk = osc;
	m->speed = osc*(mf+1);
	m->cpuhz = m->speed*MHz;	/* general system clock (cycles) */
	m->clockgen = osc*MHz;		/* clock generator frequency (cycles) */

	*(ushort*)&(io->memc[4].base) = 0x8060;
//	*(ushort*)&(io->memc[7].option) = 0xffe0;
	*(ushort*)&(io->memc[7].base) = 0xff00;

}


void
bootargs(ulong base)
{
print("bootargs = %ulx\n", base);
	USED(base);
}

void
init0(void)
{
	int i;

	up->nerrlab = 0;

print("spllo = %ux\n", spllo());

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	up->dot = cclone(up->slash, 0);

	chandevinit();

	if(!waserror()){
		ksetenv("cputype", "power");
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
	print("to user!!\n");
	touser((void*)(USTKTOP-8));
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
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->procmode = 0640;

	strcpy(p->text, "*init*");
	strcpy(p->user, eve);
	p->fpstate = FPinit;

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

	for(;;)
		;
//	arch->reset();
}

/*
 *  set up floating point for a new process
 */
void
procsetup(Proc *p)
{
	USED(p);
}

/*
 *  Save the mach dependent part of the process state.
 */
void
procsave(Proc *p)
{
	USED(p);
}

// print without using interrupts
void
serialputs(char *s, int n)
{
	putstrn(s, n);
}

void
rdb(void)
{
	panic("rdb");
}

void
confinit(void)
{
	int nbytes;
	ulong pa;

	conf.nmach = 1;		/* processors */
	conf.nproc = 60;	/* processes */

	// hard wire for now
	pa = 0xffd00000;		// leave 1 Meg for kernel
	nbytes = 2*1024*1024;	// leave room at the top as well
	
	conf.npage0 = nbytes/BY2PG;
	conf.base0 = pa;
	
	pa = 0xfff04000;
	nbytes = 1024*1024 - 0x4000;

	conf.npage1 = nbytes/BY2PG;
	conf.base1 = pa;

	conf.npage = conf.npage0 + conf.npage1;

	conf.upages = (conf.npage*60)/100;
	conf.ialloc = ((conf.npage-conf.upages)/2)*BY2PG;

	/* set up other configuration parameters */
	conf.nswap = 0; // conf.npage*3;
	conf.nswppo = 0; // 4096;
	conf.nimage = 20;

	conf.copymode = 0;		/* copy on write */
}

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

static char BOOTARGS[] = 
		"ether0=type=SCC port=1 ea=000086353a6b\r\n"
		"ether1=type=589E port=0x300\r\n";

static void
options(void)
{
	long i, n;
	char *cp, *p, *q;
	char *line[MAXCONF];
	Bootargs *ba;

	/*
	 *  parse configuration args from dos file plan9.ini
	 */
	ba = (Bootargs*)(NVRAMMEM+ 4*1024);
	if(ba->chksum == nvcsum(ba->args, sizeof(ba->args))) {
		cp = smalloc(strlen(ba->args)+1);
		memmove(cp, ba->args, strlen(ba->args)+1);
	} else
		cp = BOOTARGS;
print("bootargs = %s\n", cp);

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

int
isaconfig(char *class, int ctlrno, ISAConf *isa)
{
	char cc[NAMELEN], *p, *q, *r;
	int n;

	sprint(cc, "%s%d", class, ctlrno);
	for(n = 0; n < nconf; n++){
		if(strncmp(confname[n], cc, NAMELEN))
			continue;
		isa->nopt = 0;
		p = confval[n];
		while(*p){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == '\0')
				break;
			if(strncmp(p, "type=", 5) == 0){
				p += 5;
				for(q = isa->type; q < &isa->type[NAMELEN-1]; q++){
					if(*p == '\0' || *p == ' ' || *p == '\t')
						break;
					*q = *p++;
				}
				*q = '\0';
			}
			else if(strncmp(p, "port=", 5) == 0)
				isa->port = strtoul(p+5, &p, 0);
			else if(strncmp(p, "irq=", 4) == 0)
				isa->irq = strtoul(p+4, &p, 0);
			else if(strncmp(p, "mem=", 4) == 0)
				isa->mem = strtoul(p+4, &p, 0);
			else if(strncmp(p, "size=", 5) == 0)
				isa->size = strtoul(p+5, &p, 0);
			else if(strncmp(p, "freq=", 5) == 0)
				isa->freq = strtoul(p+5, &p, 0);
			else if(strncmp(p, "dma=", 4) == 0)
				isa->dma = strtoul(p+4, &p, 0);
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

