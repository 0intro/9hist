#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"
#include	"pool.h"

Conf	conf;
FPsave	initfp;

void
main(void)
{
	conf.nmach = 1;
	machinit();
	active.machs = 1;
	active.exiting = 0;
	i8250console();
	print("\nPlan 9\n");
//	cpuidentify();
//	confinit();
//	xinit();
	trapinit();
print("trapinit done\n");
*(ulong*)-1 = 0;
print("survived!\n");
spllo();
putdec(64);
print("still here\n");
{ void (*f)(void); f = (void (*)(void))0x200; f(); }
for(;;);
//	printinit();
//	cpuidprint();
//	mmuinit();
//	procinit0();
//	initseg();
//	links();
//	chandevreset();
//	pageinit();
//	swapinit();
//	userinit();
//	schedinit();
}

void
machinit(void)
{
	memset(m, 0, sizeof(Mach));

	/*
	 * For polled uart output at boot, need
	 * a default delay constant. 100000 should
	 * be enough for a while. Cpuidentify will
	 * calculate the real value later.
	 */
//	m->loopconst = 100000;
}

static struct
{
	char	*name;
	char *val;
}
plan9ini[] =
{
	{ "console", "0" },
};

char*
getconf(char *name)
{
	int i;

	for(i = 0; i < nelem(plan9ini); i++)
		if(cistrcmp(name, plan9ini[i].name) == 0)
			return plan9ini[i].val;
	return nil;
}

void
init0(void)
{
//	char **p, *q, name[KNAMELEN];
//	int n;

print("init0\n");

	up->nerrlab = 0;

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	cnameclose(up->slash->name);
	up->slash->name = newcname("/");
	up->dot = cclone(up->slash);

	chandevinit();

	if(!waserror()){
		ksetenv("cputype", "power");
		if(cpuserver)
			ksetenv("service", "cpu");
		else
			ksetenv("service", "terminal");
		
/*
		for(p = confenv; *p; p++) {
			q = strchr(p[0], '=');
			if(q == 0)
				continue;
			n = q-p[0];
			if(n >= KNAMELEN)
				n = KNAMELEN-1;
			memmove(name, p[0], n);
			name[n] = 0;
			ksetenv(name, q+1);
		}
*/
		poperror();
	}
	kproc("alarm", alarmkproc, 0);
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

void
confinit(void)
{
	int nbytes;
	ulong pa;

	conf.nmach = 1;		/* processors */
	conf.nproc = 60;	/* processes */

	// hard wire for now
	pa = 0x200000;		// leave 2 Meg for kernel
	nbytes = 8*1024*1024;	// leave room at the top as well
	
	conf.npage0 = nbytes/BY2PG;
	conf.base0 = pa;
	
	conf.npage1 = 0;
	conf.base1 = pa;

	conf.npage = conf.npage0 + conf.npage1;

	conf.upages = (conf.npage*60)/100;
	conf.ialloc = ((conf.npage-conf.upages)/2)*BY2PG;

	/* set up other configuration parameters */
	conf.nswap = 0;
	conf.nswppo = 0; 
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

int
isaconfig(char *class, int ctlrno, ISAConf *isa)
{
	int i;
	char cc[KNAMELEN], *p;

	sprint(cc, "%s%d", class, ctlrno);

	p = getconf(cc);
	if(p == 0)
		return 0;
	isa->nopt = tokenize(p, isa->opt, NISAOPT);
	for(i = 0; i < isa->nopt; i++){
		p = isa->opt[i];
		if(cistrncmp(p, "type=", 5) == 0)
			isa->type = p + 5;
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
	}
	return 1;
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
