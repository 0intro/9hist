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
	Tmode=	0x43,		/* mode port (control word register) */
	T2ctl=	0x61,		/* counter 2 control port */

	/* commands */
	Latch0=	0x00,		/* latch counter 0's value */
	Load0l=	0x10,		/* load counter 0's lsb */
	Load0m=	0x20,		/* load counter 0's msb */
	Load0=	0x30,		/* load counter 0 with 2 bytes */
	Latch2=	0x80,		/* latch counter 2's value */
	Load2l=	0x90,		/* load counter 2's lsb */
	Load2m=	0xa0,		/* load counter 2's msb */
	Load2=	0xb0,		/* load counter 2 with 2 bytes */

	/* 8254 read-back command: everything > pc-at has an 8254 */
	Rdback=	0xc0,		/* readback counters & status */
	Rdnstat=0x10,		/* don't read status */
	Rdncnt=	0x20,		/* don't read counter value */
	Rd0cntr=0x02,		/* read back for which counter */
	Rd1cntr=0x04,
	Rd2cntr=0x08,

	/* modes */
	ModeMsk=0xe,
	Square=	0x6,		/* periodic square wave */
	Trigger=0x0,		/* interrupt on terminal count */
	Sstrobe=0x8,		/* software triggered strobe */

	/* counter 2 controls */
	C2gate=	0x1,
	C2speak=0x2,
	C2out=	0x10,

	Freq=	1193182,	/* Real clock frequency */

	FreqMul=16,		/* extra accuracy in fastticks/Freq calculation; ok up to ~8ghz */
};

static struct
{
	Lock	lock;
	vlong	when;		/* next fastticks a clock interrupt should occur */
	long	fastperiod;	/* fastticks/hz */
	long	fast2freq;	/* fastticks*FreqMul/Freq */
}i8253;

void
i8253init(int aalcycles, int havecycleclock)
{
	int cpufreq, loops, incr, x, y;
	vlong a, b;
	static int initialised;

	if(initialised == 0){
		initialised = 1;
		ioalloc(T0cntr, 4, 0, "i8253");
		ioalloc(T2ctl, 1, 0, "i8253.cntr2ctl");

		/*
		 *  set clock for 1/HZ seconds
		 */
		outb(Tmode, Load0|Square);
		outb(T0cntr, (Freq/HZ));	/* low byte */
		outb(T0cntr, (Freq/HZ)>>8);	/* high byte */

		/*
		 * Introduce a little delay to make sure the count is
		 * latched and the timer is counting down; with a fast
		 * enough processor this may not be the case.
		 * The i8254 (which this probably is) has a read-back
		 * command which can be used to make sure the counting
		 * register has been written into the counting element.
		 */
		x = (Freq/HZ);
		for(loops = 0; loops < 100000 && x >= (Freq/HZ); loops++){
			outb(Tmode, Latch0);
			x = inb(T0cntr);
			x |= inb(T0cntr)<<8;
		}
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
		if(havecycleclock)
			rdmsr(0x10, &a);
		x = inb(T0cntr);
		x |= inb(T0cntr)<<8;
		aamloop(loops);
		outb(Tmode, Latch0);
		if(havecycleclock)
			rdmsr(0x10, &b);
		y = inb(T0cntr);
		y |= inb(T0cntr)<<8;
		x -= y;
	
		if(x < 0)
			x += Freq/HZ;

		if(x > Freq/(3*HZ))
			break;
	}

	/*
 	 *  figure out clock frequency and a loop multiplier for delay().
	 *  n.b. counter goes up by 2*Freq
	 */
	cpufreq = loops*((aalcycles*2*Freq)/x);
	m->loopconst = (cpufreq/1000)/aalcycles;	/* AAM+LOOP's for 1 ms */

	if(havecycleclock){

		/* counter goes up by 2*Freq */
		b = (b-a)<<1;
		b *= Freq;
		b /= x;

		/*
		 *  round to the nearest megahz
		 */
		m->cpumhz = (b+500000)/1000000L;
		m->cpuhz = b;
	} else {
		/*
		 *  add in possible 0.5% error and convert to MHz
		 */
		m->cpumhz = (cpufreq + cpufreq/200)/1000000;
		m->cpuhz = cpufreq;
	}

outb(Tmode, Load0|Trigger);
outb(T0cntr, (Freq/HZ));	/* low byte */
outb(T0cntr, (Freq/HZ)>>8);	/* high byte */
}

static vlong lastfast;

int
i8253readcnt(int cntr)
{
	int v;

	ilock(&i8253.lock);
	if(cntr == 2){
		outb(Tmode, Rdback|Rd2cntr);
		v = inb(T2cntr) << 16;
		v |= inb(T2cntr);
		v |= inb(T2cntr) << 8;
	}else if(cntr == 0){
		outb(Tmode, Rdback|Rd0cntr);
		v = inb(T0cntr) << 16;
		v |= inb(T0cntr);
		v |= inb(T0cntr) << 8;
	}else if(cntr == 3){
		vlong nf = fastticks(nil);
		long set;

		set = (long)(nf - lastfast) * 100 / (long)((vlong)m->cpuhz * 100 / Freq);
		set = (Freq/HZ) - set;
		set -= 3 - 1;	/* outb, outb, wait - outb(mode) */
		outb(Tmode, Rdback|Rdnstat|Rd0cntr);
		v = inb(T0cntr);
		v |= inb(T0cntr) << 8;
		v = set - v;
	}else if(cntr == 4){
		vlong nf = fastticks(nil);
		long set;

		set = (long)(nf - lastfast) * 16 / (long)((vlong)m->cpuhz * 16 / Freq);
		set = (Freq/HZ) - set;
		set -= 3 - 1;	/* outb, outb, wait - outb(mode) */
		outb(Tmode, Rdback|Rdnstat|Rd0cntr);
		v = inb(T0cntr);
		v |= inb(T0cntr) << 8;
		v = set - v;
	}else{
		vlong nf = fastticks(nil);
		long set;

		set = (nf - lastfast) * Freq / m->cpuhz;
		set = (Freq/HZ) - set;
		set -= 3 - 1;	/* outb, outb, wait - outb(mode) */
		outb(Tmode, Rdback|Rdnstat|Rd0cntr);
		v = inb(T0cntr);
		v |= inb(T0cntr) << 8;
		v = set - v;
	}
	iunlock(&i8253.lock);
	return v;
}

static void
clockintr0(Ureg* ureg, void *v)
{
	vlong now;
	long set;

	now = fastticks(nil);
	while(i8253.when < now)
		i8253.when += i8253.fastperiod;
	set = (long)(i8253.when - now) * FreqMul / i8253.fast2freq;
	set -= 3;	/* three cycles for the count to take effect: outb, outb, wait */
lastfast = now;
	outb(T0cntr, set);	/* low byte */
	outb(T0cntr, set>>8);	/* high byte */

	checkcycintr(ureg, v);
	clockintr(ureg, v);
}

void
i8253enable(void)
{
	i8253.when = fastticks(nil);
	i8253.fastperiod = (m->cpuhz + HZ/2) / HZ;
	i8253.fast2freq = (vlong)m->cpuhz * FreqMul / Freq;
	intrenable(IrqCLOCK, clockintr0, 0, BUSUNKNOWN, "clock");
}

/*
 *  return time elapsed since clock start in
 *  100 times hz
 */
uvlong
i8253read(uvlong *hz)
{
	if(hz)
		*hz = HZ*100;
	return m->ticks*100;
}
