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

static ulong delayloop = 1000;

/*
 *  delay for l milliseconds more or less.  delayloop is set by
 *  clockinit() to match the actual CPU speed.
 */
void
delay(int l)
{
	ulong i;

	while(l-- > 0)
		for(i=0; i < delayloop; i++)
			;
}

static void
clock(Ureg *ur, void *arg)
{
	Proc *p;
	int nrun = 0;
	int islow = 0;

	USED(arg);

	m->ticks++;

	uartclock();
	checkalarms();
	hardclock();

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
 *  experimentally determined
 */
enum
{
	mul386=	380,
	mul486=	121,
	mul586=	121,
};

void
printcpufreq(void)
{
	ulong freq;
	int cpu;

	switch(cpu = x86()){
	default:
	case 586:
		freq = mul586;
		break;
	case 486:
		freq = mul486;
		break;
	case 386:
		freq = mul386;
		break;
	}
	freq *= delayloop;
	freq /= 10000;
	print("CPU is a %ud MHz %d\n", freq, cpu);
}

void
clockinit(void)
{
	ulong x, y;	/* change in counter */

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
	 *  measure time for delay(10) with current delayloop count
	 */
	outb(Tmode, Latch0);
	x = inb(T0cntr);
	x |= inb(T0cntr)<<8;
	delay(10);
	outb(Tmode, Latch0);
	y = inb(T0cntr);
	y |= inb(T0cntr)<<8;
	x -= y;

	/*
	 *  fix count
	 */
	delayloop = (delayloop*1193*10)/x;
	if(delayloop == 0)
		delayloop = 1;
}
