#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	"ureg.h"

void (*kproftimer)(ulong);

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


/*
 *  delay for l milliseconds more or less.  delayloop is set by
 *  clockinit() to match the actual CPU speed.
 */
void
delay(int l)
{
	ulong i, j;

	j = m->delayloop;
	while(l-- > 0)
		for(i=0; i < j; i++)
			;
}

void
microdelay(int l)
{
	ulong i, j;

//	j = m->delayloop/1000;
j = 10000;
	while(l-- > 0)
		for(i=0; i < j; i++)
			;
}

void
clockinit(void)
{
m->delayloop = 250*1000;	/* BUG */
#ifdef	NOTYET
	long x;

	m->delayloop = m->speed*100;
	do {
		x = rdcount();
		delay(10);
		x = rdcount() - x;
	} while(x < 0);

	/*
	 *  fix count
	 */
	m->delayloop = (m->delayloop*m->speed*1000*10)/x;
	if(m->delayloop == 0)
		m->delayloop = 1;

/*	wrcompare(rdcount()+(m->speed*1000000)/HZ); */
#endif
}

void
clock(Ureg *ur)
{
	Clock0link *lp;
	static int count;

	/* HZ == 100, timer == 1024Hz.  error < 1ms */
	count += 100;
	if (count < 1024)
		return;
	count -= 1024;

	m->ticks++;
	if(m->proc)
		m->proc->pc = ur->pc;

	accounttime();

	if(kproftimer != nil)
		kproftimer(ur->pc);

	if((active.machs&(1<<m->machno)) == 0)
		return;

	if(active.exiting && (active.machs & (1<<m->machno))) {
		print("someone's exiting\n");
		exit(0);
	}

	checkalarms();
	if(m->machno == 0){
		lock(&clock0lock);
		for(lp = clock0link; lp; lp = lp->link)
			lp->clock();
		unlock(&clock0lock);
	}

	if(up == 0 || up->state != Running)
		return;

	if(anyready())
		sched();

	/* user profiling clock */
	if(ur->status & UMODE) {
		(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);
		segclock(ur->pc);
	}
}

vlong
fastticks(uvlong *hz)
{
	if (hz)
		*hz = 100;
	return m->ticks;
}
