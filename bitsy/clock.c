#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

typedef struct OSTimer
{
	ulong	osmr[4];	/* match registers */
	ulong	oscr;		/* counter register */
	ulong	ossr;		/* status register */
	ulong	ower;		/* watchdog enable register */
	ulong	oier;		/* timer interrupt enable register */
} OSTimer;

static OSTimer *timerregs = (OSTimer*)OSTIMERREGS;
static int clockinited;

static void	clockintr(Ureg*, void*);

void
clockinit(void)
{
	/* map the clock registers */
	timerregs = mapspecial(OSTIMERREGS, 32);

	/* enable interrupts on match register 0, turn off all others */
	timerregs->ossr |= 1<<0;
	intrenable(IRQtimer0, clockintr, nil, "clock");
	timerregs->oier = 1<<0;

	/* post interrupt 1/HZ secs from now */
	timerregs->osmr[0] = timerregs->oscr + ClockFreq/HZ;

	clockinited = 1;
}

vlong
fastticks(uvlong *hz)
{
	if(hz != nil)
		*hz = ClockFreq;
	return timerregs->oscr;
}

static void
clockintr(Ureg *ureg, void*)
{
	/* reset previous interrupt */
	timerregs->ossr |= 1<<0;

	/* post interrupt 1/HZ secs from now */
	timerregs->osmr[0] = timerregs->oscr + ClockFreq/HZ;

	portclock(ureg);
}

void
delay(int ms)
{
	ulong start;
	int i;

	if(clockinited){
		while(ms-- > 0){
			start = timerregs->oscr;
			while(timerregs->oscr-start < ClockFreq/1000)
				;
		}
	} else {
		while(ms-- > 0){
			for(i = 0; i < 1000; i++)
				;
		}
	}
}

void
µdelay(int µs)
{
	ulong start;
	int i;

	if(clockinited){
		start = timerregs->oscr;
		while(timerregs->oscr - start < (µs*ClockFreq)/1000000)
			;
	} else {
		while(µs-- > 0){
			for(i = 0; i < 10; i++)
				;
		}
	}
}
