#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"
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
QLock rtclock;	/* mutex on clock operations */


enum{
	Qrtc = 1,
	Qnvram,

	Seconds=	0x00,
	Minutes=	0x02,
	Hours=		0x04, 
	Mday=		0x07,
	Month=		0x08,
	Year=		0x09,
	Status=		0x0A,

	Nvsize = 4096,
};

#define	NRTC	2
Dirtab rtcdir[]={
	"nvram",	{Qnvram, 0},	Nvsize,	0664,
	"rtc",		{Qrtc, 0},	0,	0664,
};

ulong rtc2sec(Rtc*);
void sec2rtc(ulong, Rtc*);
int *yrsize(int);

void
rtcreset(void)
{
}

void
rtcinit(void)
{
}

Chan*
rtcattach(char *spec)
{
	return devattach('r', spec);
}

Chan*
rtcclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
rtcwalk(Chan *c, char *name)
{
	return devwalk(c, name, rtcdir, NRTC, devgen);
}

void	 
rtcstat(Chan *c, char *dp)
{
	devstat(c, dp, rtcdir, NRTC, devgen);
}

Chan*
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
	return devopen(c, omode, rtcdir, NRTC, devgen);
}

void	 
rtccreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void	 
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

long	 
rtctime(void)
{
	Rtc rtc;
	int x;

	/*
	 *  read and convert from bcd
	 */
	x = splhi();
	rtc.sec = bcd2binary(Seconds);
	rtc.min = bcd2binary(Minutes);
	rtc.hour = bcd2binary(Hours);
	rtc.mday = bcd2binary(Mday);
	rtc.mon = bcd2binary(Month);
	rtc.year = bcd2binary(Year);
	splx(x);

	/*
	 *  the world starts jan 1 1970
	 */
	if(rtc.year < 70)
		rtc.year += 2000;
	else
		rtc.year += 1900;
	return rtc2sec(&rtc);
}

long	 
rtcread(Chan *c, void *buf, long n, ulong offset)
{
	ulong t, ot;
	uchar *f, *to, *e;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, rtcdir, NRTC, devgen);

	switch(c->qid.path){
	case Qrtc:
		t = rtctime();
		do{
			ot = t;
			t = rtctime();	/* make sure there's no skew */
		}while(t != ot);
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
	*(uchar*)Rtcdata = (val % 10) | (((val / 10) % 10)<<4);
}


long	 
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
		binary2bcd(Year, rtc.year);
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

void	 
rtcremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void	 
rtcwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

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
int *
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
ulong
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
void
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
