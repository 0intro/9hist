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
typedef struct Partition	Partition;
typedef struct Ptable		Ptable;

enum
{
	/* ports */
	Pbase=		0x1F0,
	Pdata=		0,	/* data port (16 bits) */
	Perror=		1,	/* error port (read) */
	Pprecomp=	1,	/* buffer mode port (write) */
	Pcount=		2,	/* sector count port */
	Psector=	3,	/* sector number port */
	Pcyllsb=	4,	/* least significant byte cylinder # */
	Pcylmsb=	5,	/* most significant byte cylinder # */
	Pdh=		6,	/* drive/head port */
	Pstatus=	7,	/* status port (read) */
	 Sbusy=		 (1<<7),
	 Sready=	 (1<<6),
	 Sdrq=		 (1<<3),
	 Serr=		 (1<<0),
	Pcmd=		7,	/* cmd port (write) */

	/* commands */
	Crecal=		0x10,
	Cread=		0x20,
	Cwrite=		0x30,
	Cident=		0xEC,
	Csetbuf=	0xEF,

	/* file types */
	Qdir=		0,
	Qdata=		(1<<(3+3)),
	Qpart=		(2<<(3+3)),
	Qmask=		(3<<(3+3)),

	Maxxfer=	4*1024,		/* maximum transfer size/cmd */
	Npart=		8+1,		/* 8 sub partitions and one for the disk */
};
#define PART(x)		((x)&0x3)
#define DRIVE(x)	(((x)>>3)&0x7)
#define MKQID(t,d,p)	((t) | ((d)<<3) | (p))

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

struct Partition
{
	ulong	start;
	ulong	end;
	uchar	name[NAMELEN];
};

struct Ptable 
{
	uchar		magic[4];	/* ascii "disk" */
	uchar		n[4];		/* number of partitions */
	struct
	{
		uchar	start[4];	/* starting block */
		uchar	end[4];		/* ending block */
		uchar	name[NAMELEN];
	} p[1];
};

/*
 *  a hard drive
 */
struct Drive
{
	Controller *cp;
	int	drive;
	int	confused;	/* needs to be recalibrated (or worse) */
	int	online;
	int	npart;		/* number of real partitions */
	Partition p[Npart];

	ulong	cap;		/* total bytes */
	int	bytes;		/* bytes/sector */
	int	sectors;	/* sectors/track */
	int	heads;		/* heads/cyl */
	long	cyl;		/* cylinders/drive */

	Ident	id;		/* disk properties */
};

/*
 *  a controller for 2 drives
 */
struct Controller
{
	QLock;			/* exclusive access to the drive */

	int	confused;	/* needs to be recalibrated (or worse) */
	int	pbase;		/* base port */

	/*
	 *  current operation
	 */
	int	cmd;		/* current command */
	Rendez	r;		/* wait here for command termination */
	char	*buf;		/* xfer buffer */
	int	tcyl;		/* target cylinder */
	int	thead;		/* target head */
	int	tsec;		/* target sector */
	int	tbyte;		/* target byte */
	int	len;		/* length of transfer (bytes) */
	int	secs;		/* sectors to be xferred */
	int	sofar;		/* bytes transferred so far */
	int	status;
	int	error;
	Drive	*dp;		/* drive being accessed */
};

Controller	*hardc;
Drive		*hard;
Dirtab 		*harddir;
#define NHDIR	2	/* directory entries/drive */

static void	hardintr(Ureg*);
static long	hardxfer(Drive*, int, void*, long, long);
static long	hardident(Drive*);
static void	hardsetbuf(Drive*, int);
static void	hardpart(Drive*);

static int
hardgen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dirp)
{
	Qid qid;
	int drive;
	char *name;
	Drive *dp;
	Partition *pp;
	ulong l;

	qid.vers = 0;
	drive = s/(Npart+1);
	s = s % (Npart+1);
	if(drive >= conf.nhard)
		return -1;
	dp = &hard[drive];

	if(s == 0){
		sprint(name, "hd%dparition", drive);
		qid.path = MKQID(Qpart, drive, 0);
		l = dp->npart * sizeof(Partition);
	} else if(s-1 < p.npart){
		pp = &dp->p[s-1];
		sprint(name, "hd%d%s", drive, pp->name);
		qid.path = MKQID(Qdata, drive, s-1);
		l = (pp->end - pp->start) * dp->cp->bytes;
	} else
		return 0;

	devdir(c, qid, name, l, 0600, dirp);
	return 1;
}
/*
 *  we assume drives 0 and 1 are on the first controller, 2 and 3 on the
 *  second, etc.
 */
void
hardreset(void)
{
	Drive *dp;
	Controller *cp;
	int drive;
	Dirtab *dir;

	hard = ialloc(conf.nhard * sizeof(Drive), 0);
	hardc = ialloc(((conf.nhard+1)/2 + 1) * sizeof(Controller), 0);
	dir = harddir = ialloc(NHDIR * conf.nhard * sizeof(Dirtab), 0);
	
	for(drive = 0; drive < conf.nhard; drive++){
		dp = &hard[drive];
		cp = &hardc[drive/2];
		dp->drive = drive&1;
		dp->online = 0;
		dp->cp = cp;
		if((drive&1) == 0){
			cp->buf = ialloc(Maxxfer, 0);
			cp->cmd = 0;
			cp->pbase = Pbase + (cp-hardc)*8;	/* BUG!! guessing */
			setvec(Hardvec + (cp-hardc)*8, hardintr); /* BUG!! guessing */
		}
		sprint(harddir[drive*2].name, "hd%ddata", drive);
		dir->length = 0;
		dir->qid.path = Qdata + drive;
		dir->perm = 0600;
		dir++;
		sprint(dir->name, "hd%dstruct", drive);
		dir->length = 8;
		dir->qid.path = Qstruct + drive;
		dir->perm = 0600;
		dir++;
	}
}

void
hardinit(void)
{
}

/*
 *  Get the characteristics of each drive.  Mark unresponsive ones
 *  off line.
 */
Chan*
hardattach(char *spec)
{
	Drive *dp;

	for(dp = hard; dp < &hard[conf.nhard]; dp++){
		if(!waserror()){
			dp->bytes = 512;
			hardsetbuf(dp, 1);
			hardident(dp);
			hardpart(dp);
			dp->cyl = dp->id.lcyls;
			dp->heads = dp->id.lheads;
			dp->sectors = dp->id.ls2t;
			dp->bytes = 512;
			dp->cap = dp->bytes * dp->cyl * dp->heads * dp->sectors;
			harddir[NHDIR*dp->drive].length = dp->cap;
			dp->online = 1;
			poperror();
		} else
			dp->online = 0;
	}

	return devattach('h', spec);
}

Chan*
hardclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
hardwalk(Chan *c, char *name)
{
	return devwalk(c, name, harddir, conf.nhard*NHDIR, devgen);
}

void
hardstat(Chan *c, char *dp)
{
	devstat(c, dp, harddir, conf.nhard*NHDIR, devgen);
}

Chan*
hardopen(Chan *c, int omode)
{
	return devopen(c, omode, harddir, conf.nhard*NHDIR, devgen);
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
		return devdirread(c, a, n, harddir, conf.nhard*NHDIR, devgen);

	rv = 0;
	dp = &hard[c->qid.path & ~Qmask];
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
	dp = &hard[c->qid.path & ~Qmask];
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
cmddone(void *a)
{
	Controller *cp = a;

	return cp->cmd == 0;
}

/*
 *  wait for the controller to be ready to accept a command
 */
static void
cmdreadywait(Controller *cp)
{
	long start;

	start = m->ticks;
	while((inb(cp->pbase+Pstatus) & (Sready|Sbusy)) != Sready)
		if(TK2MS(m->ticks - start) > 1){
print("cmdreadywait failed\n");
			errors("disk not responding");
		}
}

/*
 *  start a disk transfer.  hardintr will performa all the iterative
 *  parts.
 */
static long
hardxfer(Drive *dp, int cmd, void *va, long off, long len)
{
	Controller *cp;
	int err;
	int lsec;
	int cyl;

	if(dp->online == 0)
		errors("disk offline");
	if(len % dp->bytes)
		errors("bad length");	/* BUG - this shouldn't be a problem */
	if(off % dp->bytes)
		errors("bad offset");	/* BUG - this shouldn't be a problem */

	cp = dp->cp;
	qlock(cp);
	if(waserror()){
		qunlock(cp);
		nexterror();
	}

	/*
	 *  calculate the physical address of off
	 */
	lsec = off/dp->bytes;
	cp->tcyl = lsec/(dp->sectors*dp->heads);
	cp->tsec = (lsec % dp->sectors) + 1;
	cp->thead = (lsec/dp->sectors) % dp->heads;

	/*
	 *  can't xfer across cylinder boundaries.
	 */
	lsec = (off+len)/dp->bytes;
	cyl = lsec/(dp->sectors*dp->heads);
	if(cyl == cp->tcyl)
		cp->len = len;
	else
		cp->len = cyl*dp->sectors*dp->heads*dp->bytes - off;

	cmdreadywait(cp);

	/*
	 *  start the transfer
	 */
	cp->secs = cp->len/dp->bytes;
	cp->sofar = 0;
	cp->cmd = cmd;
	cp->dp = dp;
	outb(cp->pbase+Pcount, cp->secs);
	outb(cp->pbase+Psector, cp->tsec);
	outb(cp->pbase+Pdh, (dp->drive<<4) | cp->thead);
	outb(cp->pbase+Pcyllsb, cp->tcyl);
	outb(cp->pbase+Pcylmsb, cp->tcyl>>8);
	outb(cp->pbase+Pcmd, cmd);

	if(cmd == Cwrite){
		memmove(cp->buf, va, cp->len);
		outss(Pdata, cp->buf, dp->bytes/2);
	}
	sleep(&cp->r, cmddone, cp);
	if(cp->status & Serr){
print("hd%d err: status %lux, err %lux\n", dp-hard, cp->status, cp->error);
print("\ttcyl %d, tsec %d, thead %d\n", cp->tcyl, cp->tsec, cp->thead);
print("\tsecs %d, sofar %d\n", cp->secs, cp->sofar);
		errors("disk I/O error");
	}
	if(cmd == Cread)
		memmove(va, cp->buf, cp->len);

	poperror();
	qunlock(cp);
	return cp->len;
}

/*
 *  set read ahead mode (1 == on, 0 == off)
 */
static void
hardsetbuf(Drive *dp, int on)
{
	Controller *cp = dp->cp;

	qlock(cp);
	if(waserror()){
		qunlock(cp);
		nexterror();
	}

	cmdreadywait(cp);

	outb(cp->pbase+Pprecomp, on ? 0xAA : 0x55);
	outb(cp->pbase+Pdh, (dp->drive<<4));
	outb(cp->pbase+Pcmd, Csetbuf);

	sleep(&cp->r, cmddone, cp);

	poperror();
	qunlock(cp);
}

/*
 *  get parameters from the drive
 */
static long
hardident(Drive *dp)
{
	Controller *cp;

	cp = dp->cp;
	qlock(cp);
	if(waserror()){
		qunlock(cp);
		nexterror();
	}

	cmdreadywait(cp);

	cp->len = 512;
	cp->secs = 1;
	cp->sofar = 0;
	cp->cmd = Cident;
	cp->dp = dp;
	outb(cp->pbase+Pdh, (dp->drive<<4));
	outb(cp->pbase+Pcmd, Cident);
	sleep(&cp->r, cmddone, cp);
	if(cp->status & Serr){
print("bad disk magic\n");
		errors("disk I/O error");
	}
	memmove(&dp->id, cp->buf, cp->len);

	if(dp->id.magic != 0xA5A){
print("bad disk magic\n");
		errors("bad disk magic");
	}

	poperror();
	qunlock(cp);
}

/*
 *  read partition table
 */
static void
hardpart(Drive *dp)
{
	Partition *pp;
	Ptable *pt;
	uchar buf[1024];

	pp = &dp->p[0];
	strcpy(pp->name, "disk");
	pp->start = 0;
	pp->end = pp->cap / dp->bytes;

	qlock(dp->cp);
	hardxfer(dp, Cread, buf, pp->end - 1, dp->bytes);
	
	qunlock(dp->cp);
}

/*
 *  we get an interrupt for every sector transferred
 */
static void
hardintr(Ureg *ur)
{
	Controller *cp;
	long loop;

	/*
 	 *  BUG!! if there is ever more than one controller, we need a way to
	 *	  distinguish which interrupted
	 */
	cp = &hardc[0];

	cp->status = inb(cp->pbase+Pstatus);
	switch(cp->cmd){
	case Cwrite:
		if(cp->status & Serr){
			cp->cmd = 0;
			cp->error = inb(cp->pbase+Perror);
			wakeup(&cp->r);
			return;
		}
		cp->sofar++;
		if(cp->sofar != cp->secs){
			while((inb(cp->pbase+Pstatus) & Sdrq) == 0)
				if(++loop > 10000)
					panic("hardintr 1");
			outss(cp->pbase+Pdata, &cp->buf[cp->sofar*cp->dp->bytes],
				cp->dp->bytes/2);
		} else{
			cp->cmd = 0;
			wakeup(&cp->r);
		}
		break;
	case Cread:
	case Cident:
		if(cp->status & Serr){
			cp->cmd = 0;
			cp->error = inb(cp->pbase+Perror);
			wakeup(&cp->r);
			return;
		}
		loop = 0;
		while((inb(cp->pbase+Pstatus) & Sdrq) == 0)
			if(++loop > 10000)
				panic("hardintr 2");
		inss(cp->pbase+Pdata, &cp->buf[cp->sofar*cp->dp->bytes],
			cp->dp->bytes/2);
		cp->sofar++;
		if(cp->sofar == cp->secs){
			cp->cmd = 0;
			wakeup(&cp->r);
		}
		break;
	default:
		print("wierd disk interrupt\n");
		break;
	}
}
