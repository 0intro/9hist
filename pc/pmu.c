#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/*
 *  intel power management unit (i80c51)
 */
enum {
	/*
	 *  power management unit ports
	 */
	Pmudata=	0x198,

	Pmucsr=		0x199,
	 Busy=		0x1,

	/*
	 *  configuration port
	 */
	Pconfig=	0x3F3,
};

/*
 *  return when pmu ready
 */
static int
pmuready(void)
{
	int tries;

	for(tries = 0; (inb(Pmucsr) & Busy); tries++)
		if(tries > 1000)
			return -1;
	return 0;
}

/*
 *  return when pmu busy
 */
static int
pmubusy(void)
{
	int tries;

	for(tries = 0; !(inb(Pmucsr) & Busy); tries++)
		if(tries > 1000)
			return -1;
	return 0;
}

/*
 *  set a bit in the PMU
 */
Lock pmulock;
int
pmuwrbit(int index, int bit, int pos)
{
	lock(&pmulock);
	outb(Pmucsr, 0x02);		/* next is command request */
	if(pmuready() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmudata, (2<<4) | index);	/* send write bit command */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmucsr, 0x01);		/* next is data */
	if(pmuready() < 0){
		unlock(&pmulock);
		return -1;
	}
	outb(Pmudata, (bit<<3) | pos);	/* send bit to write */
	outb(Pmucsr, 0x01);		/* send available */
	if(pmubusy() < 0){
		unlock(&pmulock);
		return -1;
	}
	unlock(&pmulock);
	return 0;
}

/*
 *  power to serial port
 *	onoff == 1 means on
 *	onoff == 0 means off
 */
int
pmuserial(int onoff)
{
	return pmuwrbit(1, 1^onoff, 6);
}

/*
 *  power to modem
 *	onoff == 1 means on
 *	onoff == 0 means off
 */
int
pmumodem(int onoff)
{
	if(pmuwrbit(1, 1^onoff, 0)<0)		/* modem speaker */
		return -1;
	return pmuwrbit(1, onoff, 5);		/* modem power */
}

/*
 *  set cpu speed
 * 	0 == low speed
 *	1 == high speed
 */
int
pmucpuspeed(int speed)
{
	return pmuwrbit(0, speed, 0);
}

/*
 *  f == frequency in Hz
 *  d == duration in ms
 */
void
pmubuzz(int f, int d)
{
	static QLock bl;
	static Rendez br;

	USED(f);
	qlock(&bl);
	pmuwrbit(0, 0, 6);
	tsleep(&br, return0, 0, d);
	pmuwrbit(0, 1, 6);
	qunlock(&bl);
}

/*
 *  1 == owl eye
 *  2 == mail icon
 */
void
pmulights(int val)
{
	pmuwrbit(0, (val&1), 4);		/* owl */
	pmuwrbit(0, ((val>>1)&1), 1);		/* mail */
}
