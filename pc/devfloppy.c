#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

typedef	struct Drive	Drive;
typedef	struct Controller	Controller;
typedef struct Type	Type;

enum
{
	Fmotor=		0x3f2,	/* motor port */
	 Fintena=	 0x4,	/* enable floppy interrupt */
	 Fena=		 0x8,	/* 0 == reset controller */

	Fstatus=	0x3f4,	/* controller main status port */
	 Fready=	 0x80,	/* ready to be touched */
	 Ffrom=		 0x40,	/* data from controller */
	 Fbusy=		 0x10,	/* operation not over */

	Fdata=		0x3f5,	/* controller data port */
	 Frecal=	 0x7,	/* recalibrate cmd */
	 Fseek=		 0xf,	/* seek cmd */
	 Fsense=	 0x8,	/* sense cmd */
	 Fread=		 0x66,	/* read cmd */
	 Fwrite=	 0x47,	/* write cmd */
	 Fmulti=	 0x80,	/* or'd with Fread or Fwrite for multi-head */

	Ndrive=	4,	/* floppies/controller */

	DMAchan=	2,	/* floppy dma channel */

	/* sector size encodings */
	S128=		0,
	S256=		1,
	S512=		2,
	S1024=		3,

	/* status 0 byte */
	Drivemask=	3<<0,
	Seekend=	1<<5,
	Codemask=	(3<<6)|(3<<3),

	/* file types */
	Qdir=		0,
	Qdata=		1,
	Qstruct=	2,
};

/*
 *  floppy types
 */
struct Type
{
	char	*name;
	int	bytes;		/* bytes/sector */
	int	sectors;	/* sectors/track */
	int	heads;		/* number of heads */
	int	steps;		/* steps per cylinder */
	int	tracks;		/* tracks/disk */
	int	gpl;		/* intersector gap length for read/write */	
	int	fgpl;		/* intersector gap length for format */

	/*
	 *  these depend on previous entries and are set filled in
	 *  by floppyinit
	 */
	int	bcode;		/* coded version of bytes for the controller */
	long	cap;		/* drive capacity in bytes */
};
Type floppytype[] =
{
	{ "MF2HD",	512,	18,	2,	1,	80,	0x1B,	0x54, },
	{ "MF2DD",	512,	9,	2,	1,	80,	0x1B,	0x54, },
	{ "F2HD",	512,	15,	2,	1,	80,	0x2A,	0x50, },
	{ "F2DD",	512,	8,	2,	2,	40,	0x2A,	0x50, },
	{ "F1DD",	512,	8,	1,	2,	40,	0x2A,	0x50, },
};
#define NTYPES (sizeof(floppytype)/sizeof(Type))

/*
 *  bytes/sector encoding for the controller, index is (bytes per sector/128)
 */
static int b2c[] =
{
[1]	0,
[2]	1,
[4]	2,
[8]	3,
};
static int c2b[] =
{
	128,
	256,
	512,
	1024,
};

/*
 *  a floppy drive
 */
struct Drive
{
	QLock;			/* exclusive access to the drive */

	Type	*t;
	int	dev;

	ulong	lasttouched;	/* time last touched */
	int	motoron;	/* motor is on */
	int	cyl;		/* current cylinder */
	long	offset;		/* current offset */
	int	confused;	/* needs to be recalibrated (or worse) */

	int	tcyl;		/* target cylinder */
	int	thead;		/* target head */
	int	tsec;		/* target sector */
	long	len;		/* size of xfer */

	int	busy;		/* true if drive is seeking */
	Rendez	r;		/* waiting here for motor to spin up */
};

/*
 *  NEC PD765A controller for 4 floppys
 */
struct Controller
{
	QLock;			/* exclusive access to the contoller */

	Drive	d[Ndrive];	/* the floppy drives */
	int	busy;		/* true if a read or write in progress */
	uchar	stat[8];	/* status of an operation */
	int	confused;
	int	intr;		/* true if interrupt occured */
	Rendez	r;		/* wait here for command termination */
};

Controller	floppy;

/*
 *  predeclared
 */
static void	motoron(Drive*);
static void	motoroff(Drive*);
static void	floppykproc(void*);
static int	floppysend(int);
static int	floppyrcv(void);
static int	floppyresult(int);
static void	floppypos(Drive*);
static int	floppysense(Drive*);
static int	interrupted(void*);
static int	floppyrecal(Drive*);
static void	floppyrevive(void);
static long	floppyseek(Drive*);
static long	floppyxfer(Drive*, int, void*, long);
static void	floppyintr(Ureg*);

static int
floppygen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dp)
{
	long l;
	Drive *dp;

	if(s >= ntab)
		return -1;
	if(c->dev >= Ndrive)
		return -1;

	tab += s;
	dp = &floppy.d[c->dev];
	if((tab->qid.path&~Mask) == Qdata)
		l = dp->t->cap;
	else
		l = 8;
	devdir(c, tab->qid, tab->name, l, tab->perm, dp);
	return 1;
}

void
floppyreset(void)
{
	Drive *dp;

	for(dp = floppy.d; dp < &floppy.d[Ndrive]; dp++){
		dp->dev = dp - floppy.d;
		dp->t = &floppytype[0];		/* default type */
		dp->motoron = 1;
		dp->cyl = -1;
		motoroff(dp);
	}
	setvec(Floppyvec, floppyintr);
}

void
floppyinit(void)
{
	Type *t;

	/*
	 *  init dependent parameters
	 */
	for(t = floppytype; t < &floppytype[NTYPES], t++){
		t->cap = t->bytes * t->heads * t->sectors * t->tracks;
		t->bcode = bcode[t->bytes/128];
	}

	/*
	 *  watchdog to turn off the motors
	 */
	kproc(floppykproc, 0);
}
long
floppyread(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i;
	uchar *aa = a;

	dp = &floppy.d[c->dev];
	for(rv = 0; rv < n; rv += i){
		i = floppyxfer(dp, Fread, aa+rv, n-rv);
		if(i <= 0)
			break;
	}
	return rv;
}

long
floppywrite(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i;
	uchar *aa = a;

	dp = &floppy.d[c->dev];
	for(rv = 0; rv < n; rv += i){
		i = floppyxfer(dp, Fwrite, aa+rv, n-rv);
		if(i <= 0)
			break;
	}
	return rv;
}

/*
 *  start a floppy drive's motor.  set an alarm for 1 second later to
 *  mark it as started (we get no interrupt to tell us).
 *
 *  assume the caller qlocked the drive.
 */
static void
motoron(Drive *dp)
{
	int cmd;

	cmd = (1<<(dp->dev+4)) | Fintena | Fena | dp->dev;
	outb(Fmotor, cmd);
	dp->busy = 1;
	tsleep(&dp->r, noreturn, 0, 1000);
	dp->motoron = 1;
	dp->busy = 0;
	dp->lasttouched = m->ticks;
}

/*
 *  stop the floppy if it hasn't been used in 5 seconds
 */
static void
motoroff(Drive *dp)
{
	int cmd;

	cmd = Fintena | Fena | dp->dev;
	outb(Fmotor, cmd);
	dp->motoron = 0;	
}
static void
floppykproc(void *a)
{
	Drive *dp;

	for(dp = floppy.d; dp < &floppy.d[Ndrive]; dp++){
		if(dp->motoron && TK2SEC(m->ticks - dp->lasttouched) > 5
		&& canqlock(dp)){
			if(TK2SEC(m->ticks - dp->lasttouched) > 5)
				motoroff(dp);
			qunlock(dp);
		}
	}
}

/*
 *  send a byte to the floppy
 */
static int
floppysend(int data)
{
	int tries;
	uchar c;

	for(tries = 0; tries < 100; tries++){
		/*
		 *  see if its ready for data
		 */
		c = inb(Fstatus);
		if((c&(Ffrom|Fready)) != Fready)
			continue;

		/*
		 *  send the data
		 */
		outb(Fdata, data);
		return 0;
	}
	return -1;
}

/*
 *  get a byte from the floppy
 */
static int
floppyrcv(void)
{
	int tries;
	uchar c;

	for(tries = 0; tries < 100; tries++){
		/*
		 *  see if its ready for data
		 */
		c = inb(Fstatus);
		if((c&(Ffrom|Fready)) != (Ffrom|Fready))
			continue;

		/*
		 *  get data
		 */
		return inb(Fdata)&0xff;
	}
	return -1;
}

/*
 *  read a command result message from the floppy
 */
static int
floppyresult(int n)
{
	int i;
	int c;

	for(i = 0; i < n; i++){
		c = floppyrcv();
		if(c < 0)
			return -1;
		floppy.stat[i] = c;
	}
	return 0;
}

/*
 *  calculate physical address of a logical byte offset into the disk
 *
 *  truncate dp->length if it crosses a cylinder boundary
 */
static void
floppypos(Drive *dp)
{
	int lsec;
	int end;
	int cyl;

	lsec = dp->off/dp->t->bytes;
	dp->tcyl = lsec/(dp->t->sectors*dp->t->heads);
	dp->tsec = (lsec % dp->t->sectors) + 1;
	dp->thead = (lsec/dp->t->sectors) % dp->t->heads;

	/*
	 *  can't read across cylinder boundaries.
	 *  if so, decrement the bytes to be read.
	 */
	lsec = (dp->off+dp->len)/dp->t->bytes;
	cyl = lsec/(dp->t->sectors*dp->t->heads);
	if(cyl != dp->tcyl){
		dp->len -= (lsec % dp->t->sectors)*dp->t->bytes;
		dp->len -= ((lsec/dp->t->sectors) % dp->t->heads)*dp->t->bytes
				*dp->t->sectors;
	}
}

/*
 *  get the interrupt cause from the floppy.  we need to do this
 *  after seeks and recalibrations since they don't return results.
 */
static int
floppysense(Drive *dp)
{
	/*
	 *  ask for floppy status
	 */
	if(floppysend(Fsense) < 0){
		floppy.confused = 1;
		return -1;
	}
	if(floppyresult(2) < 0){
		floppy.confused = 1;
		dp->confused = 1;
		return -1;
	}

	/*
	 *  make sure it's the right drive
	 */
	if((floppy.stat[0] & Drivemask) != dp-floppy.d){
		print("sense failed\n");
		dp->confused = 1;
		return -1;
	}
	return 0;
}

/*
 *  return true if interrupt occurred
 */
static int
interrupted(void *a)
{
	return floppy.intr;
}

/*
 *  we've lost the floppy position, go to cylinder 0.
 */
static int
floppyrecal(Drive *dp)
{
	floppy.intr = 0;
	if(floppysend(Frecal) < 0
	|| floppysend(dp - floppy.d) < 0){
		floppy.confused = 0;
		return -1;
	}
	sleep(&floppy.r, interrupted, 0);

	/*
	 *  get return values
	 */
	if(floppysense(dp) < 0)
		return -1;

	/*
	 *  see if it worked
	 */
	if((floppy.stat[0] & (Codemask|Seekend)) != Seekend){
		print("recalibrate failed\n");
		dp->confused = 1;
		return -1;
	}

	/*
	 *  see what cylinder we got to
	 */
	dp->tcyl = 0;
	dp->cyl = floppy.stat[1]/dp->t->steps;
	if(dp->cyl != dp->tcyl){
		print("recalibrate went to wrong cylinder %d\n", dp->cyl);
		dp->confused = 1;
		return -1;
	}

	dp->confused = 0;
	return 0;
}

/*
 *  if the controller or a specific drive is in a confused state,
 *  reset it and get back to a kown state
 */
void
floppyrevive(void)
{
	Drive *dp;

	/*
	 *  reset the floppy if it's confused
	 */
	if(floppy.confused){
		/* reset controller and turn all motors off */
		floppy.intr = 0;
		splhi();
		outb(Fmotor, 0);
		delay(1);
		outb(Fmotor, Fintena|Fena);
		spllo();
		for(dp = floppy.d; dp < &floppy.d[Ndrive]; dp++){
			dp->motoron = 0;
			dp->confused = 1;
		}
		sleep(&floppy.r, interrupted, 0);
		floppy.confused = 0;
	}

	/*
	 *  recalibrate any confused drives
	 */
	for(dp = floppy.d; floppy.confused == 0 && dp < &floppy.d[Ndrive]; dp++){
		if(dp->confused == 0)
			floppyrecal(dp);

	}

}

static long
floppyseek(Drive *dp)
{
	if(dp->cyl == dp->tcyl){
		dp->offset = off;
		return off;
	}

	/*
	 *  tell floppy to seek
	 */
	if(floppysend(Fseek) < 0
	|| floppysend((dp->thead<<2) | dp->dev) < 0
	|| floppysend(dp->tcyl * dp->t->steps) < 0){
		print("seek cmd failed\n");
		floppy.confused = 1;
		return -1;
	}

	/*
	 *  wait for interrupt
	 */
	sleep(&floppy.r, interrupted, 0);

	/*
	 *  get floppy status
	 */
	if(floppysense(dp) < 0)
		return -1;

	/*
 	 *  see if it worked
	 */
	if((floppy.stat[0] & (Codemask|Seekend)) != Seekend){
		print("seek failed\n");
		dp->confused = 1;
		return -1;
	}

	/*
	 *  see what cylinder we got to
	 */
	dp->cyl = floppy.stat[1]/dp->t->steps;
	if(dp->cyl != dp->tcyl){
		print("seek went to wrong cylinder %d instead of %d\n", dp->cyl, dp->tcyl);
		dp->confused = 1;
		return -1;
	}

	dp->offset = off;
	return dp->offset;
}

static long
floppyxfer(Drive *dp, int cmd, void *a, long n)
{
	ulong addr;
	long offset;

	addr = (ulong)a;

	qlock(&floppy);
	qlock(dp);
	if(waserror){
		qunlock(&floppy);
		qunlock(dp);
	}

	/*
	 *  get floppy reset and spinning
	 */
	if(floppy.confused || dp->confused)
		floppyrevive();
	if(!dp->motoron)
		motoron(dp);

	/*
	 *  calculate new position and seek to it (dp->len may be trimmed)
	 */
	dp->len = n;
	floppypos(dp);
	if(floppyseek(dp) < 0)
		errors("seeking floppy");

print("tcyl %d, thead %d, tsec %d, addr %lux, n %d\n",
		dp->tcyl, dp->thead, dp->tsec, addr, n);/**/

	/*
	 *  set up the dma (dp->len may be trimmed)
	 */
	dp->len = dmasetup(2, a, dp->len, cmd==Fread);

	/*
	 *  start operation
	 */
	cmd = cmd | (dp->t->heads > 1 ? Fmulti : 0);
	if(floppysend(cmd) < 0
	|| floppysend((dp->thead<<2) | dev) < 0
	|| floppysend(dp->tcyl * dp->t->steps) < 0
	|| floppysend(dp->thead) < 0
	|| floppysend(dp->tsec) < 0
	|| floppysend(dp->t->bcode) < 0
	|| floppysend(dp->t->sectors) < 0
	|| floppysend(dp->t->gpl) < 0
	|| floppysend(0xFF) < 0){
		print("xfer cmd failed\n");
		floppy.confused = 1;
		errors("floppy command failed");
	}

	sleep(&floppy.r, interrupted, 0);

	/*
	 *  get status
 	 */
	if(floppyresult(7) < 0){
print("xfer status failed\n");
		floppy.confused = 1;
		errors("floppy result failed");
	}

	if((floppy.stat[0] & Codemask)!=0 || floppy.stat[1] || floppy.stat[2]){
print("xfer failed %lux %lux %lux\n", floppy.stat[0],
			floppy.stat[1], floppy.stat[2]);
		dp->confused = 1;
		errors("floppy drive lost");
	}

	offset = (floppy.stat[3]/dp->t->steps) * dp->t->heads + floppy.stat[4];
	offset = offset*dp->t->sectors + floppy.stat[5] - 1;
	offset = offset * c2b[floppy.stat[6]];
	if(offset != dp->offset+n){
print("new offset %d instead of %d\n", offset, dp->offset+dp->len);
		dp->confused = 1;
		errors("floppy drive lost");
	}

	qunlock(&floppy);
	qunlock(dp);
	poperror();

	dp->offset += dp->len;
	return dp->len;
}

static void
floppyintr(Ureg *ur)
{
	floppy.intr = 1;
	wakeup(&floppy.r);
}
