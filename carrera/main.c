#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"init.h"

/*
 *  args passed by boot process
 */
int _argc; char **_argv; char **_env;

/*
 *  arguments passed to initcode and /boot
 */
char argbuf[128];

/*
 *  environment passed to boot -- sysname, consname, diskid
 */
char consname[NAMELEN];
char bootdisk[NAMELEN];
char screenldepth[NAMELEN];

/*
 * software tlb simulation
 */
Softtlb stlb[MAXMACH][STLBSIZE];

Conf	conf;
FPsave	initfp;

void
main(void)
{
	tlbinit();		/* Very early to establish IO mappings */
	ioinit();
	arginit();
	confinit();
	savefpregs(&initfp);
	machinit();
	kmapinit();
	xinit();
	iomapinit();
	printinit();
	serialinit();
	vecinit();
	iprint("\n\nBrazil\n");
screeninit(); 
	pageinit();
	procinit0();
	initseg();
	chandevreset();
	rootfiles();
	swapinit();
	userinit();
	schedinit();
}

/*
 *  copy arguments passed by the boot kernel (or ROM) into a temporary buffer.
 *  we do this because the arguments are in memory that may be allocated
 *  to processes or kernel buffers.
 *
 *  also grab any environment variables that might be useful
 */
struct
{
	char	*name;
	char	*val;
}bootenv[] =
{
	{"netaddr=",	sysname},
	{"console=",	consname},
	{"bootdisk=",	bootdisk},
	{"ldepth=",	screenldepth},
};
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
	int i, n;
	char **av;

	/*
	 *  get boot env variables
	 */
	if(*sysname == 0)
		for(av = _env; *av; av++)
			for(i=0; i < sizeof bootenv/sizeof bootenv[0]; i++){
				n = strlen(bootenv[i].name);
				if(strncmp(*av, bootenv[i].name, n) == 0){
					strncpy(bootenv[i].val, (*av)+n, NAMELEN);
					bootenv[i].val[NAMELEN-1] = '\0';
					break;
				}
			}

	/*
	 *  pack args into buffer
	 */
	av = (char**)argbuf;
	sp = argbuf + sizeof(argbuf);
	for(i = 0; i < _argc; i++){
		if(strchr(_argv[i], '='))
			break;
		av[i] = pusharg(_argv[i]);
	}
	av[i] = 0;
}

/*
 *  initialize a processor's mach structure.  each processor does this
 *  for itself.
 */
void
machinit(void)
{
	int n;

	/* Ensure CU1 is off */
	clrfpintr();

	/* scrub cache */
	cleancache();

	n = m->machno;
	m->stb = &stlb[n][0];

	m->speed = 50;
	clockinit();

	active.exiting = 0;
	active.machs = 1;
}

/*
 * Set up a console on serial port 2
 */
void
serialinit(void)
{
	NS16552setup(Uart1, UartFREQ);
	NS16552special(0, 9600, &kbdq, &printq, kbdcr2nl);

	kbdinit();
}

/*
 * Map IO address space in wired down TLB entry 1
 */
void
ioinit(void)
{
	ulong devphys, isaphys, intphys, isamphys;

	/*
	 * Map devices and the Eisa control space
	 */
	devphys = IOPTE|PPN(Devicephys);
	isaphys = IOPTE|PPN(Eisaphys);

	puttlbx(1, Devicevirt, devphys, isaphys, PGSZ64K);

	/*
	 * Map Interrupt control & Eisa memory
	 */
	intphys  = IOPTE|PPN(Intctlphys);
	isamphys = IOPTE|PPN(Eisamphys);

	puttlbx(2, Intctlvirt, intphys, isamphys, PGSZ1M);

	/* Enable all devce interrupt */
	IO(ushort, Intenareg) = 0xffff;

	/* Look at the first 16M of Eisa memory */
iprint("write latch\n");
/*	IO(uchar, EisaLatch) = 0; /**/
iprint("done\n");
}

/*
 * Pull the ethernet address out of NVRAM
 */
void
enetaddr(uchar *ea)
{
	int i;
	uchar tbuf[8];

	for(i = 0; i < 8; i++)
		tbuf[i] = ((uchar*)(NvramRO+Enetoffset))[i];

	print("ether:");
	for(i = 0; i < 6; i++) {
		ea[i] = tbuf[7-i];
		print("%2.2ux", ea[i]);
	}
	print("\n");
}

/*
 * All DMA and ether IO buffers must reside in the first 16M bytes of
 * memory to be covered by the translation registers
 */
void
iomapinit(void)
{
	int i;
	Tte *t;

	t = xspanalloc(Ntranslation*sizeof(Tte), BY2PG, 0);

	for(i = 0; i < Ntranslation; i++)
		t[i].lo = i<<PGSHIFT;

	/* Set the translation table */
	IO(ulong, Ttbr) = PADDR(t);
	IO(ulong, Tlrb) = (Ntranslation-1)*sizeof(Tte);

	/* Invalidate the old entries */
	IO(ulong, Tir) = 0;
}

/*
 *  setup MIPS trap vectors
 */
void
vecinit(void)
{
	memmove((ulong*)UTLBMISS, (ulong*)vector0, 0x100);
	memmove((ulong*)CACHETRAP, (ulong*)vector100, 0x80);
	memmove((ulong*)EXCEPTION, (ulong*)vector180, 0x80);

	icflush((ulong*)UTLBMISS, 8*1024);
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
	up->dot = clone(up->slash, 0);

	iallocinit();
	chandevinit();

	if(!waserror()){
		ksetenv("cputype", "mips");
		sprint(buf, "sgi %s 4D", conffile);
		ksetenv("terminal", buf);
		ksetenv("sysname", sysname);
		poperror();
	}

	kproc("alarm", alarmkproc, 0);
	touser((uchar*)(USTKTOP-sizeof(argbuf)));
}

void
userinit(void)
{
	Proc *p;
	KMap *k;
	Page *pg;
	char **av;
	Segment *s;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = smalloc(sizeof(Fgrp));
	p->fgrp->ref = 1;
	p->procmode = 0640;

	strcpy(p->text, "*init*");
	strcpy(p->user, eve);

	p->fpstate = FPinit;
	p->fpsave.fpstatus = initfp.fpstatus;

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

	/* Text */
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
exit(long type)
{
	USED(type);

	spllo();
	print("cpu %d exiting\n", m->machno);
	while(consactive())
		delay(10);
	splhi();
	for(;;)
		;
}

void
confinit(void)
{
	ulong ktop, top;

	ktop = PGROUND((ulong)end);
	ktop = PADDR(ktop);
	top = (16*1024*1024)/BY2PG;

	conf.base0 = 0;
	conf.npage0 = top;
	conf.npage = conf.npage0;
	conf.npage0 -= ktop/BY2PG;
	conf.base0 += ktop;
	conf.npage1 = 0;
	conf.base1 = 0;

	conf.upages = (conf.npage*70)/100;

	conf.nmach = 1;

	/* set up other configuration parameters */
	conf.nproc = 100;
	conf.nswap = conf.npage*3;
	conf.nimage = 200;
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;

	conf.monitor = 0;

	conf.copymode = 0;		/* copy on write */
}


/*
 *  for the sake of devcons
 */

void
buzz(int f, int d)
{
	USED(f);
	USED(d);
}
