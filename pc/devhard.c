#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

typedef	struct Drive		Drive;
typedef	struct Ident		Ident;
typedef	struct Controller	Controller;

enum
{
	/* ports */
	Pbase=		0x10,
	Pdata=		Pbase+0,	/* data port (16 bits) */
	Perror=		Pbase+1,	/* error port */
	Pcount=		Pbase+2,	/* sector count port */
	Psector=	Pbase+3,	/* sector number port */
	Pcyllsb=	Pbase+4,	/* least significant byte cylinder # */
	Pcylmsb=	Pbase+5,	/* most significant byte cylinder # */
	Pdh=		Pbase+6,	/* drive/head port */
	Pstatus=	Pbase+7,	/* status port */
	 Sbusy=		 (1<<7),
	 Sready=	 (1<<6),
	 Sdrq=		 (1<<5),
	 Serr=		 (1<<0),
	Pcmd=		Pbase+7,	/* cmd port */

	/* commands */
	Crecal=		0x10,
	Cread=		0x20,
	Cwrite=		0x30,
	Cident=		0xEC,

	/* file types */
	Qdir=		0,
	Qdata=		(1<<1),
	Qstruct=	(2<<1),
	Qmask=		(3<<1),
};

/*
 *  ident sector from drive
 */
struct Ident
{
	ushort	magic;		/* must be 0x0A5A */
	ushort	lcyls;		/* logical number of cylinders */
	ushort	rcyl;		/* number of removable cylinders */
	ushort	lheads;		/* logical number of heads */
	ushort	b2t;		/* unformatted bytes/track */
	ushort	b2s;		/* unformated bytes/sector */
	ushort	ls2t;		/* logical sectors/track */
	ushort	gap;		/* bytes in inter-sector gaps */
	ushort	sync;		/* bytes in sync fields */
	ushort	magic2;		/* must be 0x0000 */
	ushort	serial[10];	/* serial number */
	ushort	type;		/* controller type (0x0003) */
	ushort	bsize;		/* buffer size/512 */
	ushort	ecc;		/* ecc bytes returned by read long */
	ushort	firm[4];	/* firmware revision */
	ushort	model[20];	/* model number */
	ushort	s2i;		/* number of sectors/interrupt */
	ushort	dwtf;		/* double word transfer flag */
	ushort	alernate;
	ushort	piomode;
	ushort	dmamode;
	ushort	reserved[76];
	ushort	ncyls;		/* native number of cylinders */
	ushort	nheads;		/* native number of heads, sectors */
	ushort	dlcyls;		/* default logical number of cyinders */
	ushort	dlheads;	/* default logical number of heads, sectors */
	ushort	interface;
	ushort	power;		/* 0xFFFF if power commands supported */
	ushort	flags;
	ushort	ageprog;	/* MSB = age, LSB = program */
	ushort	reserved2[120];
};

/*
 *  a hard drive
 */
struct Drive
{
	int	dev;
	int	confused;	/* needs to be recalibrated (or worse) */
	int	online;

	ulong	cap;		/* drive capacity */
	int	bytes;		/* bytes/sector */
	int	sectors;	/* sectors/track */
	int	heads;		/* heads/cyl */
	long	cyl;		/* cylinders/drive */

	int	tcyl;		/* target cylinder */
	int	thead;		/* target head */
	int	tsec;		/* target sector */
	long	len;		/* size of xfer */
};

/*
 *  a controller for 2 drives
 */
struct Controller
{
	QLock;			/* exclusive access to the drive */

	int	intr;		/* true if interrupt occured */
	int	status;		/* status of last interupt */
	Rendez	r;		/* wait here for command termination */
	int	confused;	/* needs to be recalibrated (or worse) */

	Drive	*d;
	Ident	id;
};

Controller	hard;

Dirtab harddir[]={
	"hddata",		{Qdata},	0,	0600,
	"hdstruct",		{Qstruct},	8,	0600,
};
#define NHDIR	(sizeof(harddir)/sizeof(Dirtab))

static void	hardintr(Ureg*);
static long	hardxfer(Drive*, int, void*, long, long);
static long	hardident(Drive*);
static void	hardpos(Drive*, long);

void
hardreset(void)
{
	Drive *dp;

	hard.d = ialloc(conf.nhard * sizeof(Drive), 0);
	for(dp = hard.d; dp < &hard.d[conf.nhard]; dp++){
		dp->dev = dp - hard.d;
		dp->online = 0;
	}

	setvec(Hardvec, hardintr);
}

void
hardinit(void)
{
	qunlock(&hard);
}

Chan*
hardattach(char *spec)
{
	Drive *dp;

	qlock(&hard);
	for(dp = hard.d; dp < &hard.d[conf.nhard]; dp++){
		if(!waserror()){
			hardident(dp);
			dp->cyl = hard.id.lcyls;
			dp->heads = hard.id.lheads;
			dp->sectors = hard.id.ls2t;
			dp->bytes = 512;
			dp->cap = dp->bytes * dp->cyl * dp->heads * dp->sectors;
			dp->online = 1;
		}
	}
	return devattach('f', spec);
}

Chan*
hardclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
hardwalk(Chan *c, char *name)
{
	return devwalk(c, name, harddir, NHDIR, devgen);
}

void
hardstat(Chan *c, char *dp)
{
	devstat(c, dp, harddir, NHDIR, devgen);
}

Chan*
hardopen(Chan *c, int omode)
{
	return devopen(c, omode, harddir, NHDIR, devgen);
}

void
hardcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
hardclose(Chan *c)
{
}

void
hardremove(Chan *c)
{
	error(Eperm);
}

void
hardwstat(Chan *c, char *dp)
{
	error(Eperm);
}

static void
ul2user(uchar *a, ulong x)
{
	a[0] = x >> 24;
	a[1] = x >> 16;
	a[2] = x >> 8;
	a[3] = x;
}

long
hardread(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i;
	uchar *aa = a;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, harddir, NHDIR, devgen);

	rv = 0;
	dp = &hard.d[c->qid.path & ~Qmask];
	switch ((int)(c->qid.path & Qmask)) {
	case Qdata:
		for(rv = 0; rv < n; rv += i){
			i = hardxfer(dp, Cread, aa+rv, c->offset+rv, n-rv);
			if(i <= 0)
				break;
		}
		break;
	case Qstruct:
		if (n < 2*sizeof(ulong))
			error(Ebadarg);
		if (c->offset >= 2*sizeof(ulong))
			return 0;
		rv = 2*sizeof(ulong);
		ul2user((uchar*)a, dp->cap);
		ul2user((uchar*)a+sizeof(ulong), dp->bytes);
		break;
	default:
		panic("hardread: bad qid");
	}
	return rv;
}

long
hardwrite(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i;
	uchar *aa = a;

	rv = 0;
	dp = &hard.d[c->qid.path & ~Qmask];
	switch ((int)(c->qid.path & Qmask)) {
	case Qdata:
		for(rv = 0; rv < n; rv += i){
			i = hardxfer(dp, Cwrite, aa+rv, c->offset+rv, n-rv);
			if(i <= 0)
				break;
		}
		break;
	case Qstruct:
		error(Eperm);
		break;
	default:
		panic("hardwrite: bad qid");
	}
	return rv;
}

/*
 *  did an interrupt happen?
 */
static int
interrupted(void *a)
{
	return hard.intr;
}

/*
 *  get parameters from the drive
 */
static long
hardident(Drive *dp)
{
print("identify hard drive\n");
	hard.intr = 0;
	outb(Pdh, dp->dev<<4);
	outb(Pcmd, Cident);
print("waiting for hard drive interupt\n");
	sleep(&hard.r, interrupted, 0);
print("getting hard drive ident\n");
	inss(Pdata, &hard.id, 512/2);
print(" magic %lux lcyls %d rcyl %d lheads %d b2t %d b2s %d ls2t %d\n",
  hard.id.magic, hard.id.lcyls, hard.id.rcyl, hard.id.lheads,
  hard.id.b2t, hard.id.b2s, hard.id.ls2t);
}

static long
hardxfer(Drive *dp, int cmd, void *va, long off, long len)
{
	int secs;
	int i;
	uchar *aa = va;

	if(off % dp->bytes)
		errors("bad offset");
	if(len % dp->bytes)
		errors("bad length");

	if(waserror()){
		qunlock(&hard);
		nexterror();
	}
	qlock(&hard);
	dp->len = len;
	hardpos(dp, off);
	secs = dp->len/dp->bytes;

	outb(Pcount, secs);
	outb(Psector, dp->tsec);
	outb(Pdh, (1<<5) | (dp->dev<<4) | dp->thead);
	outb(Pcyllsb, dp->tcyl);
	outb(Pcylmsb, dp->tcyl>>8);
	outb(Pcmd, cmd);

	if(cmd == Cwrite)
		outss(Pdata, aa, dp->bytes/2);
	for(i = 0; i < secs; i++){
		hard.intr = 0;
		sleep(&hard.r, interrupted, 0);
		if(hard.status & Serr)
			errors("disk error");
		if(cmd == Cread){
			if((hard.status & Sdrq) == 0)
				panic("disk read");
			inss(Pdata, aa + i*dp->bytes, dp->bytes/2);
		} else {
			if((hard.status & Sdrq) == 0){
				if(i+1 != secs)
					panic("disk write");
			} else
				outss(Pdata, aa + (i+1)*dp->bytes, dp->bytes/2);
		}
	}
	qunlock(&hard);
}

/*
 *  take/clear a disk interrupt
 */
static void
hardintr(Ureg *ur)
{
	hard.status = inb(Pstatus);
	if(hard.status & Sbusy)
		panic("disk busy");
	hard.intr = 1;
print("hardintr\n");
	wakeup(&hard.r);
}

/*
 *  calculate physical address of a logical byte offset into the disk
 *
 *  truncate dp->len if it crosses a cylinder boundary
 */
static void
hardpos(Drive *dp, long off)
{
	int lsec;
	int end;
	int cyl;

	lsec = off/dp->bytes;
	dp->tcyl = lsec/(dp->sectors*dp->heads);
	dp->tsec = (lsec % dp->sectors) + 1;
	dp->thead = (lsec/dp->sectors) % dp->heads;

	/*
	 *  can't read across cylinder boundaries.
	 *  if so, decrement the bytes to be read.
	 */
	lsec = (off+dp->len)/dp->bytes;
	cyl = lsec/(dp->sectors*dp->heads);
	if(cyl != dp->tcyl){
		dp->len -= (lsec % dp->sectors)*dp->bytes;
		dp->len -= ((lsec/dp->sectors) % dp->heads)*dp->bytes*dp->sectors;
	}
}

