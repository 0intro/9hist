#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

static	ulong	clkreload;

// the following can be 4 or 16 depending on the clock multiplier
// see 15.3.3 in 860 manual
enum {
	Timebase = 16,	/* system clock cycles per time base cycle */
};

void
clockinit(void)
{
//	delayloopinit();

//	clkreload = (m->clockgen/Timebase)/HZ-1;
	clkreload = (300000000/Timebase)/HZ-1;
	putdec(clkreload);
}

void
clockintr(Ureg *ureg)
{
	long v;

	v = -getdec();
	if(v > clkreload/2){
		if(v > clkreload)
			m->ticks += v/clkreload;
		v = 0;
	}
	putdec(clkreload-v);

	if(m->flushmmu){
		if(up)
			flushmmu();
		m->flushmmu = 0;
	}

	portclock(ureg);
if((m->ticks%HZ) == 0) print("tick! %d\n", m->ticks/HZ);
}

void
delay(int l)
{
	ulong i, j;

	j = m->loopconst;
	while(l-- > 0)
		for(i=0; i < j; i++)
			;
}

void
microdelay(int l)
{
	ulong i;

	l *= m->loopconst;
	l += 500;
	l /= 1000;
	if(l <= 0)
		l = 1;
	for(i = 0; i < l; i++)
		;
}

vlong
fastticks(uvlong *hz)
{
	if(hz)
		*hz = HZ;
	return m->ticks;
}
