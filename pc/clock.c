#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

/*
 *  8253 timer
 */
enum
{
	T0cntr=	0x40,		/* counter ports */
	T1cntr=	0x41,		/* ... */
	T2cntr=	0x42,		/* ... */
	Tmode=	0x43,		/* mode port */

	/* commands */
	Latch0=	0x00,		/* latch counter 0's value */
	Load0=	0x30,		/* load counter 0 with 2 bytes */

	/* modes */
	Square=	0x36,		/* perioic square wave */
	Trigger= 0x30,		/* interrupt on terminal count */

	Freq=	1193182,	/* Real clock frequency */
};

static int cpufreq = 66;
static int cputype = 486;
static int loopconst = 100;

static void
clock(Ureg *ur, void *arg)
{
	Proc *p;
	int nrun = 0;
	int islow = 0;

	USED(arg);

	m->ticks++;

	checkalarms();
	hardclock();
	uartclock();

	/*
	 *  process time accounting
	 */
	p = m->proc;
	if(p){
		nrun = 1;
		p->pc = ur->pc;
		if (p->state==Running)
			p->time[p->insyscall]++;
	}
	nrun = (nrdy+nrun)*1000;
	MACHP(0)->load = (MACHP(0)->load*19+nrun)/20;

	if(up && (ur->cs&0xffff) == UESEL && up->state == Running){
		if(anyready())
			sched();
	
		/* user profiling clock */
		islow = 1;
		spllo();		/* in case we fault */
		(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);
	}

	/*
	 *  anything from here down can be running spllo()
	 */
	if(!islow){
		islow = 1;
		spllo();
	}

	mouseclock();

	if(islow)
		splhi();
}

/*
 *  delay for l milliseconds more or less.  delayloop is set by
 *  clockinit() to match the actual CPU speed.
 */
void
delay(int l)
{
	aamloop(l*loopconst);
}

void
printcpufreq(void)
{
	print("CPU is a %ud MHz %d\n", cpufreq, cputype);
}

void
clockinit(void)
{
	ulong x, y;	/* change in counter */
	ulong cycles, loops;

	/*
	 *  set vector for clock interrupts
	 */
	setvec(Clockvec, clock, 0);

	/*
	 *  set clock for 1/HZ seconds
	 */
	outb(Tmode, Load0|Square);
	outb(T0cntr, (Freq/HZ));	/* low byte */
	outb(T0cntr, (Freq/HZ)>>8);	/* high byte */

	/*
	 *  measure time for the loop
	 *
	 *			MOVL	loops,CX
	 *	aaml1:	 	AAM
	 *			LOOP	aaml1
	 *
	 *  the time for the loop should be independent from external
	 *  cache's and memory system since it fits in the execution
	 *  prefetch buffer.
	 *
	 */
	loops = 10000;
	outb(Tmode, Latch0);
	x = inb(T0cntr);
	x |= inb(T0cntr)<<8;
	aamloop(loops);
	outb(Tmode, Latch0);
	y = inb(T0cntr);
	y |= inb(T0cntr)<<8;
	x -= y;

	/*
	 *  counter  goes at twice the frequency, once per transition,
	 *  i.e., twice per the square wave
	 */
	x >>= 1;

	/*
 	 *  figure out clock frequency and a loop multiplier for delay().
	 */
	switch(cputype = x86()){
	case 386:
		cycles = 32;
		break;
	case 486:
		cycles = 22;
		break;
	default:
		cycles = 24;
		break;
	}
	cpufreq = loops*((cycles*Freq)/x);
	loopconst = (cpufreq/1000)/cycles;	/* AAM+LOOP's for 1 ms */

	/* convert to MHz */
	cpufreq = cpufreq/1000000;
}
