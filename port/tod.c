#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"


// frequency of the tod clock
#define TODFREQ	1000000000LL

enum
{
	Log2mult=	22,	// multiplier should have 22 bits
	Log2todfreq=	30,	// number of significant bits of TODFREQ
};

static vlong logtab[40];

struct {
	Lock;
	int	shift;		// time = (ticks*multiplier)>>shift
	vlong	multiplier;	// ...
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
	// this ensures that the multiplier has 22 bits
	ilock(&tod);
	tod.hz = f;
	tod.shift = Log2mult - Log2todfreq + log2(f);
	tod.multiplier = (TODFREQ<<tod.shift)/f;
	iunlock(&tod);
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
	x = (diff*tod.multiplier)>>tod.shift;
	x += tod.off;

	// protect against overflows
	if(diff > (1LL<<(63-Log2mult))){
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
	ilock(&tod);
	if(tod.n > tod.i++)
		tod.off += tod.delta;
	iunlock(&tod);
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
