#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

// compute nanosecond epoch time from the fastest ticking clock
// on the system.  converting the time to nanoseconds requires
// the following formula
//
//	t = (((1000000000<<31)/f)*ticks)>>31
//
//  where
//
//	'f'		is the clock frequency
//	'ticks'		are clock ticks
//
//  to avoid too much calculation in gettod(), we calculate
//
//	mult = (1000000000<<31)/f
//
//  each time f is set.  f is normally set by a user level
//  program writing to /dev/fastclock.


// frequency of the tod clock
#define TODFREQ	1000000000LL

struct {
	Lock;
	int	s1;		// time = ((ticks>>s2)*multiplier)>>(s1-s2)
	vlong	multiplier;	// ...
	int	s2;		// ...
	vlong	hz;		// frequency of fast clock
	vlong	last;		// last reading of fast clock
	vlong	off;		// offset from epoch to last
	vlong	lasttime;	// last return value from gettod
	vlong	delta;		// add 'delta' each slow clock tick from sstart to send
	ulong	sstart;		// ...
	ulong	send;		// ...
} tod;

void
todinit(void)
{
	fastticks((uvlong*)&tod.hz);
	todsetfreq(tod.hz);
	addclock0link(todfix);
}

//
//  calculate multiplier
//
void
todsetfreq(vlong f)
{
	// the shift is an attempt to maintain precision
	// during the caculations.  the number of bits in
	// the multiplier should be log(TODFREQ) + 31 - log(f).
	//
	//	Freq		bits
	//	167 MHZ		34
	//	267 MHZ		33
	//	500 MHZ		32
	//
	// in all cases, we need to call todget() at least once
	// a second to keep the subsequent calculations from
	// overflowing.

	ilock(&tod);
	tod.hz = f;
	tod.multiplier = (TODFREQ<<31)/f;
	iunlock(&tod);
}

//
//  Set the time of day struct
//
void
todset(vlong t, vlong delta, int n)
{
	ilock(&tod);
	tod.sstart = tod.send = 0;
	if(t >= 0){
		tod.off = t;
		tod.last = fastticks(nil);
		tod.lasttime = 0;
	} else {
		if(n <= 0)
			n = 1;
		n *= HZ;
		if(delta < 0 && n > -delta)
			n = -delta;
		if(delta > 0 && n > delta)
			n = delta;
		delta /= n;
		tod.sstart = MACHP(0)->ticks;
		tod.send = tod.sstart + n;
		tod.delta = delta;
	}
	iunlock(&tod);
}

//
//  get time of day
//
vlong
todget(void)
{
	uvlong x;
	vlong ticks, diff;
	ulong t;

	ilock(&tod);

	if(tod.hz == 0)
		ticks = fastticks((uvlong*)&tod.hz);
	else
		ticks = fastticks(nil);
	diff = ticks - tod.last;

	// add in correction
	if(tod.sstart < tod.send){
		t = MACHP(0)->ticks;
		if(t >= tod.send)
			t = tod.send;
		tod.off += tod.delta*(t - tod.sstart);
		tod.sstart = t;
	}

	// convert to epoch, make sure calculation is unsigned
	x = (((uvlong)diff) * ((uvlong)tod.multiplier)) >> 31;
	x += tod.off;

	// protect against overflows (gettod is called at least once a second)
	tod.last = ticks;
	tod.off = x;

	// time can't go backwards
	if(x < tod.lasttime)
		x = tod.lasttime;
	tod.lasttime = x;

	iunlock(&tod);
	return x;
}

//
//  called every clock tick
//
void
todfix(void)
{
	// once a second, make sure we don't overflow
	if((MACHP(0)->ticks % HZ) == 0)
		todget();
}

long
seconds(void)
{
	vlong x;
	int i;

	x = todget();
	x /= TODFREQ;
	i = x;
	return i;
}
