#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

void
clockintr(Ureg *ureg)
{
	// this needs to be here for synchronizing mp clocks
	// see pc/mp.c's squidboy()
	fastticks(nil);

	if(m->flushmmu){
		if(up)
			flushmmu();
		m->flushmmu = 0;
	}

print("portclock\n");
	portclock(ureg);
print("done portclock\n");
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

return;
	l *= m->loopconst;
	l += 500;
	l /= 1000;
	if(l <= 0)
		l = 1;
	for(i = 0; i < l; i++)
		;
}

void
clockinit(void)
{
}

vlong
fastticks(uvlong *hz)
{
	if(hz)
		*hz = HZ;
	return m->ticks;
}
