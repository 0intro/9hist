#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"
#include	"pool.h"
#include	"/sys/src/boot/alphapc/conf.h"
#include	"axp.h"

char argbuf[128];	/* arguments passed to initcode and /boot */

Hwrpb *hwrpb;
Bootconf *bootconf;
Conf	conf;
FPsave	initfp;

void
main(void)
{
	hwrpb = (Hwrpb*)0x10000000;
	hwrpb = (Hwrpb*)(KZERO|hwrpb->phys);
	arginit();
	machinit();
	clockinit();
	confinit();
	archinit();
	savefpregs(&initfp);
	mmuinit();
	xinit();
	printinit();
	if (arch->coreinit)
		arch->coreinit();
	trapinit();

	/* console */
	screeninit();
	ns16552install();
	ns16552special(0, 9600, 0, &printq, kbdcr2nl);
	kbdinit();

	cpuidprint();
	if (arch->corehello)
		arch->corehello();

#ifdef	NEVER
	percpu = hwrpb + (hwrpb[40]>>2);
//	percpu[32] |= 2;			/* restart capable */
	percpu[32] &= ~1;			/* boot in progress - not */
//	percpu[32] |= (3<<16);		/* warm boot requested */
//	percpu[32] |= (2<<16);		/* cold boot requested */
//	percpu[32] |= (4<<16);		/* stay halted */
	percpu[32] |= (0<<16);		/* default action */
#endif

	procinit0();
	initseg();
	links();
	chandevreset();
	pageinit();
	swapinit();
	userinit();
	schedinit();
}

/*
 *  initialize a processor's mach structure.  each processor does this
 *  for itself.
 */
void
machinit(void)
{
	int n;

	icflush();
	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;

	active.exiting = 0;
	active.machs = 1;
}

void
init0(void)
{
	char buf[2*NAMELEN];

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	up->dot = cclone(up->slash, 0);

	chandevinit();

	if(!waserror()){
		ksetenv("cputype", "alpha");
		sprint(buf, "alpha %s axp", conffile);
		ksetenv("terminal", buf);
		ksetenv("sysname", sysname);
		poperror();
	}

	kproc("alarm", alarmkproc, 0);
	touser((uchar*)(USTKTOP - sizeof(argbuf)));
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	KMap *k;
	char **av;
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
	p->fpsave.fpstatus = initfp.fpstatus;
	fpenab(0);

	/*
	 * Kernel Stack
	 */
	p->sched.pc = (ulong)init0;
	p->sched.sp = (ulong)p->kstack+KSTACK-(1+MAXSYSARG)*BY2WD;
	/*
	 * User Stack, pass input arguments to boot process
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKSIZE/BY2PG);
	p->seg[SSEG] = s;
	pg = newpage(1, 0, USTKTOP-BY2PG);
	segpage(s, pg);
	k = kmap(pg);
	for(av = (char**)argbuf; *av; av++)
		*av += (USTKTOP - sizeof(argbuf)) - (ulong)argbuf;

	memmove((uchar*)VA(k) + BY2PG - sizeof(argbuf), argbuf, sizeof argbuf);
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
	memmove((uchar*)VA(k), initcode, sizeof initcode);
	kunmap(k);

	ready(p);
}

void
exit(int)
{
	canlock(&active);
	active.machs &= ~(1<<m->machno);
	active.exiting = 1;
	unlock(&active);

	spllo();
	print("cpu %d exiting\n", m->machno);
	do
		delay(100);
	while(consactive());

	splhi();
	delay(1000);	/* give serial fifo time to finish flushing */
	if (arch->coredetach)
		arch->coredetach();
	firmware();
}

void
confinit(void)
{
	long mbytes;
	int mul;
	ulong ktop;

	mbytes = 50;	/* BUG FIXME */

	/*
	 * This split of memory into 2 banks fools the allocator into
	 * allocating low memory pages from bank 0 for the ethernet since
	 * it has only a 24bit address counter.
	 * Note that the rom monitor has the bottom 2 megs
	 */
	conf.npage0 = (8*1024*1024)/BY2PG;
	conf.base0 = 0;

	conf.npage1 = (mbytes-8)*1024/8;
	conf.base1 = 8*1024*1024;

	conf.npage = conf.npage0+conf.npage1;
	conf.upages = (conf.npage*70)/100;

	ktop = PGROUND((ulong)end);
	conf.ptebase = ktop;
	ktop = PADDR(ktop);
	ktop += ((mbytes+7)/8 + 2)*BY2PG;		/* space for kernel ptes */
	conf.npage0 -= ktop/BY2PG;
	conf.base0 += ktop;
	conf.mbytes = mbytes;
	conf.ialloc = ((conf.npage-conf.upages)/2)*BY2PG;

	mul = (mbytes+11)/12;
	if(mul > 2)
		mul = 2;
	conf.nmach = 1;
	conf.nproc = 20 + 50*mul;
	conf.nswap = conf.nproc*80;
	conf.nimage = 50;
	conf.copymode = 0;			/* copy on write */

	if(cpuserver)
		conf.nproc = 500;
	conf.monitor = 1;	/* BUG */
}

void
lights(int l)
{
	USED(l);
}
char *sp;

char *
pusharg(char *p)
{
	int n;

	n = strlen(p)+1;
	sp -= n;
	memmove(sp, p, n);
	return sp;
}

void
arginit(void)
{
	char **av;

	av = (char**)argbuf;
	sp = argbuf + sizeof(argbuf);
	*av++ = pusharg("boot");
	*av = 0;
}

/*
 * Q&D fake-out of plan9.ini until we resolve the booting issues
 */
char *confname[] =
{
	"ether0",
	"scsi0",
	"scsi1",
	"scsi2",
	"audio0",

};

char *confval[] =
{
	"type=2114x",
	"type=ata",
	"type=ata",
	"type=ata",
	"type=sb16",
};

int	nconf = nelem(confname);

char *
getconf(char *name)
{
	int n;

	for(n = 0; n < nconf; n++){
		if(cistrcmp(confname[n], name) == 0)
			return confval[n];
	}
	return 0;
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
