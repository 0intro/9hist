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
//	t = (((1000000000<<s1)/f)*(ticks>>s2))>>(s1-s2)
//
//  where
//
//	'f'		is the clock frequency
//	'ticks'		are clock ticks
//	's1' and 's2'	are shift ammounts to avoid 64 bit
//			overflows in the calculations
//
//  to avoid too much calculation in gettod(), we calculate
//
//	mult = (1000000000<<s1)/f
//
//  each time f is set.  f is normally set by a user level
//  program writing to /dev/fastclock.
//
//  To calculate s1 and s2, we have to avoid overflowing our
//  signed 64 bit calculations.  Also we wish to accomodate
//  15 minutes of ticks.  This gives us the following
//  constraints:
//
//	1) log2(1000000000<<s1) <= 63
//	   or s1 <= 33
//	2) accomodate 15 minutes of ticks without overflow
//	   or log2(((1000000000<<s1)/f)*((15*60*f)>>s2)) <= 63
//	   or log2(mult) + 12 + log2(f) - s2 <= 63
//	   or log2(mult) + log2(f) - 51 <= s2
//
//  by definition
//
//	3) log2(mult) = log2(1000000000) + s1 - log2(f)
//	   or log2(mult) = 30 + s1 - log2(f)
//
//  To balance the accuracy of the multiplier and the sampled
//  ticks we set
//
//	4) log2(mult) = log2(f>>s2)
//	   or log2(mult) = log2(f) - s2
//
//  Combining 2) and 4) we get
//
//	5) log2(f) - s2 + log2(f) - 51 <= s2
//	   or 2*log2(f) - 51 <= 2*s2
//	   or log2(f) - 25 <= s2
//
//  Combining 3) and 4)
//
//	6) 30 + s1 - log2(f) = log2(f) - s2
//	   or s1 = 2*log2(f) - s2 - 30
//
//  Since shifting ticks left doesn't increase accuracy, and
//  shifting 1000000000 right loses accuracy
//
//	7) s2 >= 0
//	8) s1 >= 0
//
//  As an example, that gives us the following
//
//	for f = 100, log2(f) = 7
//
//		s2 = 0
//		s1 = 0
//
//	for f = 267000000, log2(f) = 28
//
//		s2 = 3
//		s1 = 23
//
//	for f = 2000000000, log2(f) = 31
//
//		s2 = 6
//		s1 = 26
//
//	for f = 8000000000, log2(f) = 33
//
//		s2 = 8
//		s1 = 28

// frequency of the tod clock
#define TODFREQ	1000000000LL

static vlong logtab[40];

struct {
	Lock;
	int	s1;		// time = ((ticks>>s2)*multiplier)>>(s1-s2)
	vlong	multiplier;	// ...
	int	s2;		// ...
	vlong	maxdiff;	// max diff between ticks and last to avoid overflow
	vlong	hz;		// frequency of fast clock
	vlong	last;		// last reading of fast clock
	vlong	off;		// offset from epoch to last
	vlong	lasttime;	// last return value from gettod
	vlong	delta;		// amount to add to bias each slow clock tick
	int	n;		// number of times to add in delta
	int	i;		// number of times we've added in delta
} tod;

int
log2(vlong x)
{
	int i;

	for(i = 0; i < nelem(logtab); i++){
		if(x < logtab[i])
			break;
	}
	return i+8;
}

void
todinit(void)
{
	vlong v;
	int i;

	v = 1LL<<8;
	for(i = 0; i < nelem(logtab); i++){
		logtab[i] = v;
		v <<= 1;
	}
	fastticks((uvlong*)&tod.hz);
	todsetfreq(tod.hz);
	addclock0link(todfix);
}

//
//  This routine makes sure that the multiplier has
//  at least Log2mult bits to guarantee that precision.
//
void
todsetfreq(vlong f)
{
	int lf;

	// this ensures that the multiplier has 22 bits
	ilock(&tod);
	tod.hz = f;
	lf = log2(f);
	tod.s2 = lf - 25;
	if(tod.s2 < 0)
		tod.s2 = 0;
	tod.s1 = 2*lf - tod.s2 - 30;
	if(tod.s1 < 0)
		tod.s1 = 0;
	if(tod.s1 > 33)
		tod.s1 = 33;
	tod.multiplier = (TODFREQ<<tod.s1)/f;
	tod.maxdiff = 1LL<<(10 + lf);
	iunlock(&tod);
//print("hz %lld mult %lld s1 %d s2 %d maxdiff %lld\n", tod.hz, tod.multiplier, tod.s1, tod.s2, tod.maxdiff);
}

//
//  Set the time of day struct
//
void
todset(vlong t, vlong delta, int n)
{
	ilock(&tod);
	tod.i = tod.n = 0;
	if(t >= 0){
		tod.off = t;
		tod.last = fastticks(nil);
		tod.lasttime = 0;
	} else {
		tod.delta = delta;
		tod.n = n;
	}
	iunlock(&tod);
}

//
//  get time of day
//
vlong
todget(void)
{
	vlong ticks, x, diff;

	ilock(&tod);

	if(tod.hz == 0)
		ticks = fastticks((uvlong*)&tod.hz);
	else
		ticks = fastticks(nil);
	diff = ticks - tod.last;

	// convert to epoch
	x = ((diff>>tod.s2)*tod.multiplier)>>(tod.s1-tod.s2);
	x += tod.off;

	// protect against overflows
	if(diff > tod.maxdiff){
		tod.last = ticks;
		tod.off = x;
	}

	/* time can't go backwards */
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
	if((MACHP(0)->ticks % HZ) != 0)
		return;

	// once a second apply correction
	ilock(&tod);
	if(tod.n > tod.i++)
		tod.off += tod.delta;
	iunlock(&tod);

	// once a minute, make sure we don't overflow
	if((MACHP(0)->ticks % (60*HZ)) == 0)
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
