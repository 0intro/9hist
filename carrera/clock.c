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
static ulong		incr;		/* compare register increment */
static uvlong		fasthz;		/* ticks/sec of fast clock */

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

ulong lastcmp;

void
clockinit(void)
{
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

	incr = (m->speed*1000000)/HZ;
	fasthz = m->speed*1000000;
	wrcompare(lastcmp = rdcount()+incr);
}

uvlong
updatefastclock(ulong count)
{
	ulong delta;

	/* keep track of higher precision time */
	delta = count - m->lastcyclecount;
	m->lastcyclecount = count;
	m->fastclock += delta;

	return m->fastclock;
}

void
clock(Ureg *ur)
{
	Clock0link *lp;
	ulong count;

	count = rdcount();
	wrcompare(lastcmp = count+incr);
	updatefastclock(count);

	m->ticks++;
	if(m->proc)
		m->proc->pc = ur->pc;

	accounttime();

	if(kproftimer != nil)
		kproftimer(ur->pc);

	kmapinval();

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

	if(m->flushmmu){
		if(up)
			flushmmu();
		m->flushmmu = 0;
	}

	if(up == 0 || up->state != Running)
		return;

	/* user profiling clock */
	if(ur->status & KUSER) {
		(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);
		segclock(ur->pc);
	}

	if(anyready())
		sched();
}

ulong timewarp;

vlong
fastticks(uvlong *hz)
{
	uvlong cyclecount;
	ulong cnt, d;
	int x;

	x = splhi();
	cnt = rdcount();
	cyclecount = updatefastclock(cnt);
	d = lastcmp - cnt + 100;
	if(d > incr+100){
		wrcompare(lastcmp = cnt+incr);
		timewarp++;
	}
	splx(x);

	if(hz)
		*hz = fasthz;

	return (vlong)cyclecount;
}

void
idlehands(void)
{
#ifdef adsfasdf
	ulong cnt, cmp, d;
	int x;

	x = splhi();
	cnt = rdcount();
	cmp = rdcompare();
	cnt -= 100;		/* to cover for time from splhi to rdcount */
	d = cmp - cnt;
	if(d > incr+100){
		wrcompare(lastcmp = cnt+incr);
		timewarp++;
//		print("%lud %lud %d %lux\n", cnt, cmp, cnt-cmp, d);
	}
	splx(x);
#endif
}
