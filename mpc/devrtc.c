#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"io.h"

/*
 * MPC8xx real time clock
 * FADS board option switch
 * interrupt statistics
 */

enum{
	Qrtc = 1,
	Qswitch,
	Qintstat,

	/* sccr */
	RTDIV=	1<<24,
	RTSEL=	1<<23,

	/* rtcsc */
	RTE=	1<<0,
	R38K=	1<<4,
};

static	QLock	rtclock;		/* mutex on clock operations */

static Dirtab rtcdir[]={
	"rtc",		{Qrtc, 0},	12,	0666,
	"switch",	{Qswitch, 0}, 0, 0444,
	"intstat",	{Qintstat, 0}, 0, 0444,
};
#define	NRTC	(sizeof(rtcdir)/sizeof(rtcdir[0]))

static void
rtcreset(void)
{
	IMM *io;
	int n;

	io = m->iomem;
	io->rtcsck = KEEP_ALIVE_KEY;
	n = (RTClevel<<8)|RTE;
	if(m->oscclk == 5)
		n |= R38K;
	io->rtcsc = n;
	io->rtcsck = ~KEEP_ALIVE_KEY;
print("sccr=#%8.8lux plprcr=#%8.8lux\n", io->sccr, io->plprcr);
}

static Chan*
rtcattach(char *spec)
{
	return devattach('r', spec);
}

static int	 
rtcwalk(Chan *c, char *name)
{
	return devwalk(c, name, rtcdir, NRTC, devgen);
}

static void	 
rtcstat(Chan *c, char *dp)
{
	devstat(c, dp, rtcdir, NRTC, devgen);
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
	case Qswitch:
	case Qintstat:
		if(omode!=OREAD)
			error(Eperm);
		break;
	}
	return devopen(c, omode, rtcdir, NRTC, devgen);
}

static void	 
rtcclose(Chan*)
{
}

static long	 
rtcread(Chan *c, void *buf, long n, vlong offset)
{
	ulong t;
//	char *b;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, rtcdir, NRTC, devgen);

	switch(c->qid.path){
	case Qrtc:
		t = m->iomem->rtc;
		n = readnum(offset, buf, n, t, 12);
		return n;
	case Qswitch:
		return readnum(offset, buf, n, 0xf/*(m->bcsr[2]>>19)&0xF*/, 12);
	}
	error(Egreg);
	return 0;		/* not reached */
}

static long	 
rtcwrite(Chan *c, void *buf, long n, vlong offset)
{
	ulong secs;
	char *cp, *ep;
	IMM *io;

	switch(c->qid.path){
	case Qrtc:
		if(offset!=0)
			error(Ebadarg);
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
		/*
		 * set it
		 */
		io = ioplock();
		io->rtck = KEEP_ALIVE_KEY;
		io->rtc = secs;
		io->rtck = ~KEEP_ALIVE_KEY;
		iopunlock();
		return n;
	case Qswitch:
		return 0;
	}
	error(Egreg);
	return 0;		/* not reached */
}

long
rtctime(void)
{
	return m->iomem->rtc;
}

Dev rtcdevtab = {
	'r',
	"rtc",

	rtcreset,
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
