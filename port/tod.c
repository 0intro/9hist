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
	vlong	multiplier;	// t = off + (multiplier*ticks)>>31
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
	if(t >= 0){
		tod.off = t;
		tod.last = fastticks(nil);
		tod.lasttime = 0;
		tod.delta = 0;
		tod.sstart = tod.send;
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
	if(tod.sstart != tod.send){
		t = MACHP(0)->ticks;
		if(t >= tod.send)
			t = tod.send;
		tod.off += tod.delta*(t - tod.sstart);
		tod.sstart = t;
	}

	// convert to epoch
	x = (diff * tod.multiplier) >> 31;
	x += tod.off;

	// protect against overflows
	if(diff > tod.hz){
		tod.last = ticks;
		tod.off = x;
	}

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
	static ulong last;

	// once a second, make sure we don't overflow
	if(MACHP(0)->ticks - last >= HZ){
		last = MACHP(0)->ticks;
		todget();
	}
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
