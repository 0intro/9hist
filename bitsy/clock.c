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

static void	clockintr(Ureg*, void*);

typedef struct Clock0link Clock0link;
typedef struct Clock0link {
	void		(*clock)(void);
	Clock0link*	link;
} Clock0link;

static Clock0link *clock0link;
static Lock clock0lock;

void
addclock0link(void (*clock)(void))
{
	Clock0link *lp;

	if((lp = malloc(sizeof(Clock0link))) == 0){
		print("addclock0link: too many links\n");
		return;
	}
	ilock(&clock0lock);
	lp->clock = clock;
	lp->link = clock0link;
	clock0link = lp;
	iunlock(&clock0lock);
}

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
}

vlong
fastticks(uvlong *hz)
{
	if(hz != nil)
		*hz = ClockFreq;
	return timerregs->oscr;
}

static void
clockintr(Ureg*, void*)
{
	/* reset previous interrupt */
	timerregs->ossr |= 1<<0;

	/* post interrupt 1/HZ secs from now */
	timerregs->osmr[0] = timerregs->oscr + ClockFreq/HZ;

	m->ticks++;
}

void
delay(int ms)
{
	ulong start;

	while(ms-- > 0){
		start = timerregs->oscr;
		while(timerregs->oscr-start < ClockFreq/1000);
	}
}
