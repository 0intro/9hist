#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

typedef struct Rtc Rtc;
struct Rtc
{
	QLock;
	int	sec;
	int	min;
	int	hour;
	int	mday;
	int	mon;
	int	year;
};
Rtc rtc;

Dirtab rtcdir[]={
	"rtc",		{1},	0,	0600,
};

static uchar pattern[] =
{
	0xc5, 0x3a, 0xa3, 0x5c, 0xc5, 0x3a, 0xa3, 0x5c
};

/*
 *  issue pattern recognition bits to nv ram to address the
 *  real time clock
 */
rtcpattern(void)
{
	uchar *nv;
	uchar ch;
	int i, j;

	nv = RTC;

	/*
	 *  read the pattern sequence pointer to reset it
	 */
	ch = *nv;

	/*
	 *  stuff the pattern recognition codes one bit at
	 *  a time into *nv.
	 */
	for(i = 0; i < sizeof(pattern); i++){
		ch = pattern[i];
		for (j = 0; j < 8; j++){
			*nv = ch & 0x1;
			ch >>= 1;
		}
	}
}

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
	if(c->qid.path != CHDIR)
		return 0;
	if(strcmp(name, "rtc") == 0){
		c->qid.path = 1;
		return 1;
	}
	return 0;
}

void	 
rtcstat(Chan *c, char *dp)
{
	devstat(c, dp, rtcdir, 1, devgen);
}

Chan*
rtcopen(Chan *c, int omode)
{
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
rtccreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
rtcclose(Chan *c)
{
}

#define GETBCD(o) ((clock[o]&0xf) + 10*(clock[o]>>4))

long	 
rtcread(Chan *c, void *buf, long n)
{
	int i,j;
	uchar ch;
	uchar *nv;
	uchar clock[8];
	char atime[64];

	if(c->offset!=0)
		error(Ebadarg); 
	nv = RTC;

	/*
	 *  set up the pattern for the clock
	 */
	qlock(&rtc);
	rtcpattern();

	/*
	 *  read out the clock one bit at a time
	 */
	for (i = 0; i < 8; i++){
		ch = 0;
		for (j = 0; j < 8; j++)
			ch |= ((*nv & 0x1) << j);
		clock[i] = ch;
	}
	qunlock(&rtc);
	rtc.sec = GETBCD(1);
	rtc.min = GETBCD(2);
	rtc.hour = GETBCD(3);
	rtc.mday = GETBCD(5);
	rtc.mon = GETBCD(6);
	rtc.year = GETBCD(7);

	/*
	 *  the world starts jan 1 1970
	 */
	if(rtc.year < 70)
		rtc.year += 2000;
	else
		rtc.year += 1900;

	sprint(atime, "%.2d:%.2d:%.2d %d/%d/%d", rtc.hour, rtc.min, rtc.sec,
		rtc.mon, rtc.mday, rtc.year);
	i = strlen(atime);

	if(c->offset >= i)
		return 0;
	if(c->offset + n > i)
		n = i - c->offset;
	strncpy(buf, &atime[c->offset], n);

	return n;
}

static int perm[] =
{
	3, 2, 1, 6, 5, 7
};

long	 
rtcwrite(Chan *c, void *buf, long n)
{
	int i,j;
	uchar ch;
	uchar clock[8];
	uchar *nv;
	char *cp;

	if(c->offset!=0)
		error(Ebadarg);

	/*
	 *  parse (most any separator will do)
	 */
	ch = 0;
	j = 0;
	clock[0] = clock[4] = 0;
	cp = buf;
	for(i = 0; i < n; i++){
		switch(*cp){
		case ':': case ' ': case '\t': case '/': case '-':
			clock[perm[j++]] = ch;
			ch = 0;
			break;
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			ch = (ch<<4) | (*cp - '0');
			break;
		default:
			error(Ebadarg);
		}
		cp++;
	}
	clock[perm[j++]] = ch;
	if(j != 6)
		error(Ebadarg);

	/*
	 *  set up the pattern for the clock
	 */
	qlock(&rtc);
	rtcpattern();

	/*
	 *  write the clock one bit at a time
	 */
	nv = RTC;
	for (i = 0; i < 8; i++){
		ch = clock[i];
		for (j = 0; j < 8; j++){
			*nv = ch & 1;
			ch >>= 1;
		}
	}
	qunlock(&rtc);
	return n;
}

void	 
rtcremove(Chan *c)
{
	error(Eperm);
}

void	 
rtcwstat(Chan *c, char *dp)
{
	error(Eperm);
}

#define SEC2MIN 60
#define SEC2HOUR (60*SEC2MIN)
#define SEC2DAY (24*SEC2HOUR)
#define SEC2YR (365*SEC2DAY)

static	char	dmsize[12] =
{
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/*
 *  compute seconds since Jan 1 1970
 */
ulong
rtc2sec(void)
{
	ulong secs;
	int i;

	for(i = 1970; i < rtc.year; i++)
		secs += dysize(i);
	for(i = 0; i < rtc.mon-1; i++)
		secs += dmsize[i] * SEC2DAY;
	if(dysize(rtc.year)==366 && rtc.mon>2)
		secs += SEC2DAY;
	secs += (rtc.mday-1) * SEC2DAY;
	secs += rtc.hour * SEC2HOUR;
	secs += rtc.min * SEC2MIN;
	secs += rtc.sec;
	return secs;
}

/*
 *  compute rtc from seconds since Jan 1 1970
 */
sec2rtc(ulong secs)
{
	int d0, d1;
	long hms, day;

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
	rtc.sec = hms % SEC2MIN;
	d1 = hms / SEC2MIN;
	rtc.min = d1 % SEC2MIN;
	d1 /= SEC2MIN;
	rtc.hour = d1;

	/*
	 * year number
	 */
	if(day >= 0)
		for(d1 = 70; day >= dysize(d1); d1++)
			day -= dysize(d1);
	else
		for (d1 = 70; day < 0; d1--)
			day += dysize(d1-1);
	rtc.year = d1;

	/*
	 * generate month
	 */
	if(dysize(d1) == 366)
		dmsize[1] = 29;
	for(d1 = 0; d0 >= dmsize[d1]; d1++)
		d0 -= dmsize[d1];
	dmsize[1] = 28;
	rtc.mday = d0 + 1;
	rtc.mon = d1;
	return;
}

/*
 *  days to year
 */
static
dysize(int y)
{

	if((y%4) == 0)
		return 366;
	return 365;
}

