#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"io.h"

/*
 *  real time clock and non-volatile ram
 */

typedef struct Rtc	Rtc;
struct Rtc
{
	int	sec;
	int	min;
	int	hour;
	int	mday;
	int	mon;
	int	year;
};
static QLock rtclock;	/* mutex on clock operations */


enum
{
	Qrtc = 1,
	Qnvram,

	Seconds=	0x00,
	Minutes=	0x02,
	Hours=		0x04, 
	Mday=		0x07,
	Month=		0x08,
	Year=		0x09,
	Status=		0x0A,
	StatusB=	0x0B,
	StatusC=	0x0C,
	StatusD=	0x0D,

	Nvsize = 4096,

	Nclock=		6,
};

Dirtab rtcdir[]={
	"nvram",	{Qnvram, 0},	Nvsize,	0664,
	"rtc",		{Qrtc, 0},	0,	0664,
};

static ulong rtc2sec(Rtc*);
static void sec2rtc(ulong, Rtc*);

static uchar statusB;

static void
getstatusB(void)
{
	int i, x;
	uchar r;

	for(i = 0; i < 10000; i++){
		x = (*(uchar*)Rtcindex)&~0x7f;
		*(uchar*)Rtcindex = x|Status;
		r = *(uchar*)Rtcdata;
		if(r & 0x80)
			continue;

		*(uchar*)Rtcindex = x|StatusB;
		statusB = *(uchar*)Rtcdata;
		break;
	}
}

static Chan*
rtcattach(char *spec)
{
	getstatusB();
	return devattach('r', spec);
}

static int	 
rtcwalk(Chan *c, char *name)
{
	return devwalk(c, name, rtcdir, nelem(rtcdir), devgen);
}

static void	 
rtcstat(Chan *c, char *dp)
{
	devstat(c, dp, rtcdir, nelem(rtcdir), devgen);
}

static Chan*
rtcopen(Chan *c, int omode)
{
	omode = openmode(omode);
	switch(c->qid.path){
	case Qrtc:
		if(strcmp(up->user, eve)!=0 && omode!=OREAD)
			error(Eperm);
		break;
	case Qnvram:
		if(strcmp(up->user, eve)!=0)
			error(Eperm);
	}
	return devopen(c, omode, rtcdir, nelem(rtcdir), devgen);
}

static void	 
rtcclose(Chan *c)
{
	USED(c);
}

static uchar
bcd2binary(int reg)
{
	uchar x;

	x = (*(uchar*)Rtcindex)&~0x7f;
	*(uchar*)Rtcindex = x|reg;
	x = *(uchar*)Rtcdata;
	return (x&0xf) + 10*(x>>4);
}

#define GETBCD(o) ((clock[o]&0xf) + 10*(clock[o]>>4))

static void
dumprtcstatus(void)
{
	int i, x;
	uchar status[4], r;

	for(i = 0; i < 10000; i++){
		x = (*(uchar*)Rtcindex)&~0x7f;
		*(uchar*)Rtcindex = x|Status;
		r = *(uchar*)Rtcdata;
		if(r & 0x80)
			continue;

		status[0] = r;
		*(uchar*)Rtcindex = x|StatusB;
		status[1] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|StatusC;
		status[2] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|StatusD;
		status[3] = *(uchar*)Rtcdata;

		*(uchar*)Rtcindex = x|Status;
		r = *(uchar*)Rtcdata;
		if((r & 0x80) == 0)
			break;
	}

	print("RTC: %uX %uX %uX %uX\n", status[0], status[1], status[2], status[3]);
}

static long	 
_rtctime(void)
{
	Rtc rtc;
	int i, x;
	uchar clock[Nclock], r;
	int busy;

	/* don't do the read until the clock is no longer busy */
	busy = 0;
	for(i = 0; i < 10000; i++){
		x = (*(uchar*)Rtcindex)&~0x7f;
		*(uchar*)Rtcindex = x|Status;
		r = *(uchar*)Rtcdata;
		if(r & 0x80){
			busy++;
			continue;
		}

		/* read clock values */
		*(uchar*)Rtcindex = x|Seconds;
		clock[0] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|Minutes;
		clock[1] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|Hours;
		clock[2] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|Mday;
		clock[3] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|Month;
		clock[4] = *(uchar*)Rtcdata;
		*(uchar*)Rtcindex = x|Year;
		clock[5] = *(uchar*)Rtcdata;

		*(uchar*)Rtcindex = x|Status;
		r = *(uchar*)Rtcdata;
		if((r & 0x80) == 0)
			break;
	}

	if(statusB & 0x04){
		rtc.sec = clock[0];
		rtc.min = clock[1];
		rtc.hour = clock[2];
		rtc.mday = clock[3];
		rtc.mon = clock[4];
		rtc.year = clock[5];
	}
	else{
		/*
		 *  convert from BCD
		 */
		rtc.sec = GETBCD(0);
		rtc.min = GETBCD(1);
		rtc.hour = GETBCD(2);
		rtc.mday = GETBCD(3);
		rtc.mon = GETBCD(4);
		rtc.year = GETBCD(5);
	}

	/*
	 *  the world starts jan 1 1970
	 */
	rtc.year += 1970;

	return rtc2sec(&rtc);
}

static Lock rtlock;

long
rtctime(void)
{
#define notdef
#ifdef notdef
	int i;
	long t, ot;

	ilock(&rtlock);

	/* loop till we get two reads in a row the same */
	t = _rtctime();
	for(i = 0; i < 100; i++){
		ot = t;
		t = _rtctime();
		if(ot == t)
			break;
	}
	iunlock(&rtlock);

	if(i == 100) print("we are boofheads\n");

	return t;
#else
	extern ulong boottime;

	return boottime+TK2SEC(MACHP(0)->ticks);
#endif /* notdef */
}

static long	 
rtcread(Chan *c, void *buf, long n, ulong offset)
{
	ulong t;
	uchar *f, *to, *e;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, rtcdir, nelem(rtcdir), devgen);

	switch(c->qid.path){
	case Qrtc:
		qlock(&rtclock);
		t = rtctime();
dumprtcstatus();
		qunlock(&rtclock);
		n = readnum(offset, buf, n, t, 12);
		return n;
	case Qnvram:
		if(offset > Nvsize)
			return -1;
		if(offset + n > Nvsize)
			n = Nvsize - offset;
		f = (uchar*)Nvram+offset;
		to = buf;
		e = f + n;
		while(f < e)
			*to++ = *f++;
		return n;
	}
	error(Ebadarg);
	return 0;
}

static void
binary2bcd(int reg, uchar val)
{
	uchar x;

	x = (*(uchar*)Rtcindex)&~0x7f;
	*(uchar*)Rtcindex = x|reg;
	if(statusB & 0x04)
		*(uchar*)Rtcdata = val;
	else
		*(uchar*)Rtcdata = (val % 10) | (((val / 10) % 10)<<4);
}


static long	 
rtcwrite(Chan *c, void *buf, long n, ulong offset)
{
	Rtc rtc;
	ulong secs;
	char *cp, *ep;
	uchar *f, *t, *e;
	int s;

	USED(c);
	switch(c->qid.path){
	case Qrtc:
		/*
		 *  read the time
		 */
		cp = ep = buf;
		ep += n;
		while(cp < ep){
			if(*cp>='0' && *cp<='9')
				break;
			cp++;
		}
		secs = strtoul(cp, 0, 0);
		sec2rtc(secs, &rtc);
	
		/*
		 *  convert to bcd
		 */
		s = splhi();
		binary2bcd(Seconds, rtc.sec);
		binary2bcd(Minutes, rtc.min);
		binary2bcd(Hours, rtc.hour);
		binary2bcd(Mday, rtc.mday);
		binary2bcd(Month, rtc.mon);
		binary2bcd(Year, rtc.year-1970);
		splx(s);

		return n;
	case Qnvram:
		if(offset > Nvsize)
			return -1;
		if(offset + n > Nvsize)
			n = Nvsize - offset;
		t = (uchar*)Nvram+offset;
		f = buf;
		e = f + n;
		while(f < e)
			*t++ = *f++;
		return n;
	}
	error(Ebadarg);
	return 0;
}

Dev rtcdevtab = {
	devreset,
	devinit,
	rtcattach,
	devclone,
	rtcwalk,
	rtcstat,
	rtcopen,
	devcreate,
	rtcclose,
	rtcread,
	devbread,
	rtcwrite,
	devbwrite,
	devremove,
	devwstat,
};

#define SEC2MIN 60L
#define SEC2HOUR (60L*SEC2MIN)
#define SEC2DAY (24L*SEC2HOUR)

/*
 *  days per month plus days/year
 */
static	int	dmsize[] =
{
	365, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};
static	int	ldmsize[] =
{
	366, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/*
 *  return the days/month for the given year
 */
static int*
yrsize(int yr)
{
	if((yr % 4) == 0)
		return ldmsize;
	else
		return dmsize;
}

/*
 *  compute seconds since Jan 1 1970
 */
static ulong
rtc2sec(Rtc *rtc)
{
	ulong secs;
	int i;
	int *d2m;

	secs = 0;

	/*
	 *  seconds per year
	 */
	for(i = 1970; i < rtc->year; i++){
		d2m = yrsize(i);
		secs += d2m[0] * SEC2DAY;
	}

	/*
	 *  seconds per month
	 */
	d2m = yrsize(rtc->year);
	for(i = 1; i < rtc->mon; i++)
		secs += d2m[i] * SEC2DAY;

	secs += (rtc->mday-1) * SEC2DAY;
	secs += rtc->hour * SEC2HOUR;
	secs += rtc->min * SEC2MIN;
	secs += rtc->sec;

	return secs;
}

/*
 *  compute rtc from seconds since Jan 1 1970
 */
static void
sec2rtc(ulong secs, Rtc *rtc)
{
	int d;
	long hms, day;
	int *d2m;

	/*
	 * break initial number into days
	 */
	hms = secs % SEC2DAY;
	day = secs / SEC2DAY;
	if(hms < 0) {
		hms += SEC2DAY;
		day -= 1;
	}

	/*
	 * generate hours:minutes:seconds
	 */
	rtc->sec = hms % 60;
	d = hms / 60;
	rtc->min = d % 60;
	d /= 60;
	rtc->hour = d;

	/*
	 * year number
	 */
	if(day >= 0)
		for(d = 1970; day >= *yrsize(d); d++)
			day -= *yrsize(d);
	else
		for (d = 1970; day < 0; d--)
			day += *yrsize(d-1);
	rtc->year = d;

	/*
	 * generate month
	 */
	d2m = yrsize(rtc->year);
	for(d = 1; day >= d2m[d]; d++)
		day -= d2m[d];
	rtc->mday = day + 1;
	rtc->mon = d;

	return;
}
