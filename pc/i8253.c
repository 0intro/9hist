#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

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
	Square=	0x36,		/* periodic square wave */
	Trigger= 0x30,		/* interrupt on terminal count */

	Freq=	1193182,	/* Real clock frequency */
};

void
i8253init(int aalcycles)
{
	int cpufreq, loops, incr, x, y;
	static int initialised;

	if(initialised == 0){
		initialised = 1;
		/*
		 *  set clock for 1/HZ seconds
		 */
		outb(Tmode, Load0|Square);
		outb(T0cntr, (Freq/HZ));	/* low byte */
		outb(T0cntr, (Freq/HZ)>>8);	/* high byte */
	}

	/* find biggest loop that doesn't wrap */
	incr = 16000000/(aalcycles*HZ*2);
	x = 2000;
	for(loops = incr; loops < 64*1024; loops += incr) {
	
		/*
		 *  measure time for the loop
		 *
		 *			MOVL	loops,CX
		 *	aaml1:	 	AAM
		 *			LOOP	aaml1
		 *
		 *  the time for the loop should be independent of external
		 *  cache and memory system since it fits in the execution
		 *  prefetch buffer.
		 *
		 */
		outb(Tmode, Latch0);
		x = inb(T0cntr);
		x |= inb(T0cntr)<<8;
		aamloop(loops);
		outb(Tmode, Latch0);
		y = inb(T0cntr);
		y |= inb(T0cntr)<<8;
		x -= y;
	
		if(x < 0)
			x += Freq/HZ;

		if(x > Freq/(3*HZ))
			break;
	}

	/*
	 *  counter  goes at twice the frequency, once per transition,
	 *  i.e., twice per square wave
	 */
	x >>= 1;

	/*
 	 *  figure out clock frequency and a loop multiplier for delay().
	 */
	cpufreq = loops*((aalcycles*Freq)/x);
	m->loopconst = (cpufreq/1000)/aalcycles;	/* AAM+LOOP's for 1 ms */

	/*
	 *  add in possible 0.5% error and convert to MHz
	 */
	m->cpumhz = (cpufreq + cpufreq/200)/1000000;
}

void
i8253enable(void)
{
	intrenable(VectorCLOCK, clockintr, 0, BUSUNKNOWN);
}
