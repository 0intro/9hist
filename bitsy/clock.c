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
	Clock0link *lp;
	static int inclockintr;

	/* reset previous interrupt */
	timerregs->ossr |= 1<<0;

	/* post interrupt 1/HZ secs from now */
	timerregs->osmr[0] = timerregs->oscr + ClockFreq/HZ;

	m->ticks++;

	if(m->proc)
		m->proc->pc = ureg->pc;

	if(inclockintr)
		return;		/* interrupted ourself */

	inclockintr = 1;

	accounttime();
	if(kproftimer != nil)
		kproftimer(ureg->pc);

	checkalarms();
	ilock(&clock0lock);
	for(lp = clock0link; lp; lp = lp->link)
		lp->clock();
	iunlock(&clock0lock);

	if(active.exiting && (active.machs & (1<<m->machno)))
		exit(0);
	inclockintr = 0;

	if(up == 0 || up->state != Running)
		return;

	if(anyready())
		sched();
	
	if((ureg->psr & PsrMask) == PsrMusr) {
		(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);
		segclock(ureg->pc);
	}
}

void
delay(int ms)
{
	ulong start;
	int i;

	if(clockinited){
		while(ms-- > 0){
			start = timerregs->oscr;
			while(timerregs->oscr-start < ClockFreq/1000);
		}
	} else {
		while(ms-- > 0){
			for(i = 0; i < 1000; i++)
				;
		}
	}
}
