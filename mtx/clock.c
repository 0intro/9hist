#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

static	ulong	clkreload;

void
delayloopinit(void)
{
	ulong v;
	uvlong x;

	m->loopconst = 5000;
	v = getdec();
	delay(1000);
	v -= getdec();

	x = m->loopconst;
	x *= m->dechz;
	x /= v;
	m->loopconst = x;
}

void
clockinit(void)
{
	/* XXX the hardcoding of these values is WRONG */
	m->cpuhz = 300000000;
	m->bushz = 66666666;

	m->dechz = m->bushz/4;			/* true for all 604e */
	m->tbhz = m->dechz;				/* conjecture; manual says bugger all */

	delayloopinit();

	clkreload = m->dechz/HZ-1;		/* decremented at 1/4 bus clock speed */
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
