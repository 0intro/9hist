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

	Nfloppy=	4,	/* floppies/controller */

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
	Qdata=		16,
	Qstruct=	32,
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
	long	cap;		/* drive capacity in bytes */
};
Type floppytype[] =
{
 { "MF2HD",	S512,	18,	2,	1,	80,	0x1B,	0x54,	512*2*18*80 },
 { "MF2DD",	S512,	9,	2,	1,	80,	0x1B,	0x54,	512*2*9*80 },
 { "F2HD",	S512,	15,	2,	1,	80,	0x2A,	0x50,	512*15*2*80 },
 { "F2DD",	S512,	8,	2,	1,	40,	0x2A,	0x50,	512*8*2*40 },
 { "F1DD",	S512,	8,	1,	1,	40,	0x2A,	0x50,	512*8*1*40 },
};
static int secbytes[] =
{
	128,
	256,
	512,
	1024
};

/*
 *  a floppy drive
 */
struct Drive
{
	QLock;
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
	long	len;

	int	busy;		/* true if drive is seeking */
	Rendez	r;		/* waiting for operation termination */
};

/*
 *  NEC PD765A controller for 4 floppys
 */
struct Controller
{
	QLock;
	Drive	d[Nfloppy];	/* the floppy drives */
	int	busy;		/* true if a read or write in progress */
	uchar	stat[8];	/* status of an operation */
	int	confused;
};

Controller	floppy;

/*
 *  start a floppy drive's motor.  set an alarm for 1 second later to
 *  mark it as started (we get no interrupt to tell us).
 *
 *  assume the caller qlocked the drive.
 */
void
floppystart(Drive *dp)
{
	int cmd;

	dp->lasttouched = m->ticks;	
	if(dp->motoron)
		return;

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
void
floppystop(Drive *dp)
{
	int cmd;

	cmd = Fintena | Fena | dp->dev;
	outb(Fmotor, cmd);
	dp->motoron = 0;	
}
void
floppykproc(Alarm* a)
{
	Drive *dp;

	for(dp = floppy.d; dp < &floppy.d[Nfloppy]; dp++){
		if(dp->motoron && TK2SEC(m->ticks - dp->lasttouched) > 5
		&& canqlock(dp)){
			floppystop(dp);
			qunlock(dp);
		}
	}
}

int
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

int
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

int
floppyrdstat(int n)
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

void
floppypos(Drive *dp, long off)
{
	int lsec;
	int end;
	int cyl;

	lsec = off/secbytes[dp->t->bytes];
	dp->tcyl = lsec/(dp->t->sectors*dp->t->heads);
	dp->tsec = (lsec % dp->t->sectors) + 1;
	dp->thead = (lsec/dp->t->sectors) % dp->t->heads;

	/*
	 *  can't read across cylinder boundaries.
	 *  if so, decrement the bytes to be read.
	 */
	lsec = (off+dp->len)/secbytes[dp->t->bytes];
	cyl = lsec/(dp->t->sectors*dp->t->heads);
	if(cyl != dp->tcyl){
		dp->len -= (lsec % dp->t->sectors)*secbytes[dp->t->bytes];
		dp->len -= ((lsec/dp->t->sectors) % dp->t->heads)*secbytes[dp->t->bytes]
				*dp->t->sectors;
	}

	dp->lasttouched = m->ticks;	
	floppy.intr = 0;
}

void
floppywait(void)
{
	int tries;

	for(tries = 0; tries < 100 && floppy.intr == 0; tries++)
		delay(5);
	if(tries >= 100)
		print("tired floopy\n");
	floppy.intr = 0;
}

int
floppysense(Drive *dp)
{
	/*
	 *  ask for floppy status
	 */
	if(floppysend(Fsense) < 0){
		floppy.confused = 1;
		return -1;
	}
	if(floppyrdstat(2) < 0){
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

int
floppyrecal(Drive *dp)
{
	floppy.intr = 0;
	if(floppysend(Frecal) < 0
	|| floppysend(dp - floppy.d) < 0){
		floppy.confused = 0;
		return -1;
	}
	floppywait();

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

void
floppyreset(void)
{
	Drive *dp;

	for(dp = floppy.d; dp < &floppy.d[Nfloppy]; dp++){
		dp->dev = dp - floppy.d;
		dp->t = &floppytype[0];		/* default type */
		dp->motoron = 1;
		dp->cyl = -1;
		floppystop(dp);
	}
}

void
floppyinit(void)
{
	kproc(floppykproc, 0);
}

void
floppyreset(void)
{
	Drive *dp;

	/*
	 *  reset the floppy if'n it's confused
	 */
	if(floppy.confused){
		/* reset controller and turn all motors off */
		floppy.intr = 0;
		splhi();
		outb(Fmotor, 0);
		delay(1);
		outb(Fmotor, Fintena|Fena);
		spllo();
		for(dp = floppy.d; dp < &floppy.d[Nfloppy]; dp++){
			dp->motoron = 0;
			dp->confused = 1;
		}
		floppywait();
		floppy.confused = 0;
	}

	/*
	 *  recalibrate any confused drives
	 */
	for(dp = floppy.d; floppy.confused == 0 && dp < &floppy.d[Nfloppy]; dp++){
		if(dp->confused == 0)
			floppyrecal(dp);

	}

}

long
floppyseek(int dev, long off)
{
	Drive *dp;

	dp = &floppy.d[dev];

	if(floppy.confused || dp->confused)
		floppyreset();

	floppystart(dp);

	floppypos(dp, off);
	if(dp->cyl == dp->tcyl){
		dp->offset = off;
		return off;
	}

	/*
	 *  tell floppy to seek
	 */
	if(floppysend(Fseek) < 0
	|| floppysend((dp->thead<<2) | dev) < 0
	|| floppysend(dp->tcyl * dp->t->steps) < 0){
		print("seek cmd failed\n");
		floppy.confused = 1;
		return -1;
	}

	/*
	 *  wait for interrupt
	 */
	floppywait();

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

long
floppyxfer(Drive *dp, int cmd, void *a, long n)
{
	int dev;
	ulong addr;
	long offset;

	addr = (ulong)a;
	dev = dp - floppy.d;

	/*
	 *  dma can't cross 64 k boundaries
	 */
	if((addr & 0xffff0000) != ((addr+n) & 0xffff0000))
		n -= (addr+n)&0xffff;

	dp->len = n;
	floppyseek(dev, dp->offset);

/*	print("tcyl %d, thead %d, tsec %d, addr %lux, n %d\n",
		dp->tcyl, dp->thead, dp->tsec, addr, n);/**/

	/*
	 *  set up the dma
	 */
	outb(DMAmode1, cmd==Fread ? 0x46 : 0x4a);
	outb(DMAmode0, cmd==Fread ? 0x46 : 0x4a);
	outb(DMAaddr, addr);
	outb(DMAaddr, addr>>8);
	outb(DMAtop, addr>>16);
	outb(DMAcount, n-1);
	outb(DMAcount, (n-1)>>8);
	outb(DMAinit, 2);

	/*
	 *  tell floppy to go
	 */
	cmd = cmd | (dp->t->heads > 1 ? Fmulti : 0);
	if(floppysend(cmd) < 0
	|| floppysend((dp->thead<<2) | dev) < 0
	|| floppysend(dp->tcyl * dp->t->steps) < 0
	|| floppysend(dp->thead) < 0
	|| floppysend(dp->tsec) < 0
	|| floppysend(dp->t->bytes) < 0
	|| floppysend(dp->t->sectors) < 0
	|| floppysend(dp->t->gpl) < 0
	|| floppysend(0xFF) < 0){
		print("xfer cmd failed\n");
		floppy.confused = 1;
		return -1;
	}

	floppywait();

	/*
	 *  get status
 	 */
	if(floppyrdstat(7) < 0){
		print("xfer status failed\n");
		floppy.confused = 1;
		return -1;
	}

	if((floppy.stat[0] & Codemask)!=0 || floppy.stat[1] || floppy.stat[2]){
print("xfer failed %lux %lux %lux\n", floppy.stat[0],
			floppy.stat[1], floppy.stat[2]);
		dp->confused = 1;
		return -1;
	}

	offset = (floppy.stat[3]/dp->t->steps) * dp->t->heads + floppy.stat[4];
	offset = offset*dp->t->sectors + floppy.stat[5] - 1;
	offset = offset * secbytes[floppy.stat[6]];
	if(offset != dp->offset+n){
		print("new offset %d instead of %d\n", offset, dp->offset+dp->len);
		dp->confused = 1;
		return -1;/**/
	}

	dp->offset += dp->len;
	return dp->len;
}

long
floppyread(int dev, void *a, long n)
{
	Drive *dp;
	long rv, i;
	uchar *aa = a;

	dp = &floppy.d[dev];
	for(rv = 0; rv < n; rv += i){
		i = floppyxfer(dp, Fread, aa+rv, n-rv);
		if(i <= 0)
			break;
	}
	return rv;
}

void
floppyintr(Ureg *ur)
{
	floppy.intr = 1;
}
