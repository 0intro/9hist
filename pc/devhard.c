#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

typedef	struct Drive		Drive;
typedef	struct Ident		Ident;
typedef	struct Controller	Controller;
typedef struct Partition	Partition;

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

	Maxxfer=	4*1024,		/* maximum transfer size/cmd */
	Npart=		8+2,		/* 8 sub partitions, disk, and partition */
};
#define PART(x)		((x)&0xF)
#define DRIVE(x)	(((x)>>4)&0x7)
#define MKQID(d,p)	(((d)<<4) | (p))

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
	char	name[NAMELEN+1];
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
	int	nsecs;		/* length of transfer (sectors) */
	int	sofar;		/* bytes transferred so far */
	int	status;
	int	error;
	Drive	*dp;		/* drive being accessed */
};

Controller	*hardc;
Drive		*hard;

static void	hardintr(Ureg*);
static long	hardxfer(Drive*, Partition*, int, long, long);
static long	hardident(Drive*);
static void	hardsetbuf(Drive*, int);
static void	hardpart(Drive*);

static int
hardgen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dirp)
{
	Qid qid;
	int drive;
	char name[NAMELEN+4];
	Drive *dp;
	Partition *pp;
	ulong l;

	qid.vers = 0;
	drive = s/Npart;
	s = s % Npart;
	if(drive >= conf.nhard)
		return -1;
	dp = &hard[drive];

	if(s >= dp->npart)
		return 0;

	pp = &dp->p[s];
	sprint(name, "hd%d%s", drive, pp->name);
	name[NAMELEN] = 0;
	qid.path = MKQID(drive, s);
	l = (pp->end - pp->start) * dp->bytes;
	devdir(c, qid, name, l, eve, 0666, dirp);
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

	hard = ialloc(conf.nhard * sizeof(Drive), 0);
	hardc = ialloc(((conf.nhard+1)/2 + 1) * sizeof(Controller), 0);
	
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
			switch(dp->id.magic){
			case 0xA5A:	/* conner drive on the AT&T NSX (safari) */
				dp->cyl = dp->id.lcyls;
				dp->heads = dp->id.lheads;
				dp->sectors = dp->id.ls2t;
				break;
			case 0x324A:	/* hard drive on the AT&T 6386 */
				dp->cyl = dp->id.lcyls - 4;
				dp->heads = dp->id.lheads;
				dp->sectors = dp->id.ls2t - 1;
				break;
			default:
				print("unknown hard disk type\n");
				error(Egreg);
			}
			dp->bytes = 512;
			dp->cap = dp->bytes * dp->cyl * dp->heads * dp->sectors;
			dp->online = 1;
			hardpart(dp);
			poperror();
		} else
			dp->online = 0;
	}

	return devattach('w', spec);
}

Chan*
hardclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
hardwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, hardgen);
}

void
hardstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, hardgen);
}

Chan*
hardopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, hardgen);
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

long
hardread(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i;
	int skip;
	uchar *aa = a;
	Partition *pp;
	Controller *cp;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, 0, 0, hardgen);

	dp = &hard[DRIVE(c->qid.path)];
	pp = &dp->p[PART(c->qid.path)];
	cp = dp->cp;

	qlock(cp);
	if(waserror()){
		qunlock(cp);
		nexterror();
	}
	skip = c->offset % dp->bytes;
	for(rv = 0; rv < n; rv += i){
		i = hardxfer(dp, pp, Cread, c->offset+rv-skip, n-rv+skip);
		if(i == 0)
			break;
		i -= skip;
		if(i > n - rv)
			i = n - rv;
		memmove(aa+rv, cp->buf + skip, i);
		skip = 0;
	}
	qunlock(cp);
	poperror();

	return rv;
}

long
hardwrite(Chan *c, void *a, long n)
{
	Drive *dp;
	long rv, i, partial;
	uchar *aa = a;
	Partition *pp;
	Controller *cp;

	if(c->qid.path == CHDIR)
		error(Eisdir);

	dp = &hard[DRIVE(c->qid.path)];
	pp = &dp->p[PART(c->qid.path)];
	cp = dp->cp;

	qlock(cp);
	if(waserror()){
		qunlock(cp);
		nexterror();
	}
	/*
	 *  if not starting on a sector boundary,
	 *  read in the first sector before writing
	 *  it out.
	 */
	partial = c->offset % dp->bytes;
	if(partial){
		hardxfer(dp, pp, Cread, c->offset-partial, dp->bytes);
		if(partial+n > dp->bytes)
			rv = dp->bytes - partial;
		else
			rv = n;
		memmove(cp->buf+partial, aa, rv);
		hardxfer(dp, pp, Cwrite, c->offset-partial, dp->bytes);
	} else
		rv = 0;

	/*
	 *  write out the full sectors
	 */
	partial = (n - rv) % dp->bytes;
	n -= partial;
	for(; rv < n; rv += i){
		i = n - rv;
		if(i > Maxxfer)
			i = Maxxfer;
		memmove(cp->buf, aa+rv, i);
		i = hardxfer(dp, pp, Cwrite, c->offset+rv, i);
		if(i == 0)
			break;
	}

	/*
	 *  if not ending on a sector boundary,
	 *  read in the last sector before writing
	 *  it out.
	 */
	if(partial){
		hardxfer(dp, pp, Cread, c->offset+rv, dp->bytes);
		memmove(cp->buf, aa+rv, partial);
		hardxfer(dp, pp, Cwrite, c->offset+rv, dp->bytes);
		rv += partial;
	}
	qunlock(cp);
	poperror();

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
			error(Eio);
		}
}

/*
 *  transfer a number of sectors.  hardintr will perform all the iterative
 *  parts.
 */
static long
hardxfer(Drive *dp, Partition *pp, int cmd, long start, long len)
{
	Controller *cp;
	int err;
	long lsec;
	int cyl;
	int loop;

	if(dp->online == 0)
		error(Eio);

	/*
	 *  cut transfer size down to disk buffer size
	 */
	start = start / dp->bytes;
	if(len > Maxxfer)
		len = Maxxfer;
	len = (len + dp->bytes - 1) / dp->bytes;

	/*
	 *  calculate physical address
	 */
	cp = dp->cp;
	lsec = start + pp->start;
	if(lsec >= pp->end)
		return 0;
	cp->tcyl = lsec/(dp->sectors*dp->heads);
	cp->tsec = (lsec % dp->sectors) + 1;
	cp->thead = (lsec/dp->sectors) % dp->heads;

	/*
	 *  can't xfer past end of disk
	 */
	if(lsec+len > pp->end)
		len = pp->end - lsec;
	cp->nsecs = len;

	cmdreadywait(cp);

	/*
	 *  start the transfer
	 */
	cp->cmd = cmd;
	cp->dp = dp;
	cp->sofar = 0;
	cp->status = 0;
/*print("xfer:\ttcyl %d, tsec %d, thead %d\n", cp->tcyl, cp->tsec, cp->thead);
print("\tnsecs %d, sofar %d\n", cp->nsecs, cp->sofar);/**/
	outb(cp->pbase+Pcount, cp->nsecs);
	outb(cp->pbase+Psector, cp->tsec);
	outb(cp->pbase+Pdh, (dp->drive<<4) | cp->thead);
	outb(cp->pbase+Pcyllsb, cp->tcyl);
	outb(cp->pbase+Pcylmsb, cp->tcyl>>8);
	outb(cp->pbase+Pcmd, cmd);

	if(cmd == Cwrite){
		loop = 0;
		while((inb(cp->pbase+Pstatus) & Sdrq) == 0)
			if(++loop > 10000)
				panic("hardxfer");
		outss(cp->pbase+Pdata, cp->buf, dp->bytes/2);
	}

	sleep(&cp->r, cmddone, cp);

	if(cp->status & Serr){
print("hd%d err: status %lux, err %lux\n", dp-hard, cp->status, cp->error);
print("\ttcyl %d, tsec %d, thead %d\n", cp->tcyl, cp->tsec, cp->thead);
print("\tnsecs %d, sofar %d\n", cp->nsecs, cp->sofar);
		error(Eio);
	}

	return cp->nsecs*dp->bytes;
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

	cp->cmd = Csetbuf;
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

	cp->nsecs = 1;
	cp->sofar = 0;
	cp->cmd = Cident;
	cp->dp = dp;
	outb(cp->pbase+Pdh, (dp->drive<<4));
	outb(cp->pbase+Pcmd, Cident);
	sleep(&cp->r, cmddone, cp);
	if(cp->status & Serr){
		print("bad disk ident status\n");
		error(Eio);
	}
	memmove(&dp->id, cp->buf, dp->bytes);

	poperror();
	qunlock(cp);
}

/*
 *  read partition table.  The partition table is just ascii strings.
 */
#define MAGIC "plan9 partitions"
static void
hardpart(Drive *dp)
{
	Partition *pp;
	Controller *cp;
	char *line[Npart+1];
	char *field[3];
	ulong n;
	int i;

	cp = dp->cp;
	qlock(cp);
	if(waserror()){
		qunlock(cp);
		print("error in hardpart\n");
		nexterror();
	}

	/*
	 *  we always have a partition for the whole disk
	 *  and one for the partition table
	 */
	pp = &dp->p[0];
	strcpy(pp->name, "disk");
	pp->start = 0;
	pp->end = dp->cap / dp->bytes;
	pp++;
	strcpy(pp->name, "partition");
	pp->start = dp->p[0].end - 1;
	pp->end = dp->p[0].end;
	dp->npart = 2;

	/*
	 *  read partition table from disk, null terminate
	 */
	hardxfer(dp, pp, Cread, 0, dp->bytes);
	cp->buf[dp->bytes-1] = 0;

	/*
	 *  parse partition table.
	 */
	n = getfields(cp->buf, line, Npart+1, '\n');
	if(strncmp(line[0], MAGIC, sizeof(MAGIC)-1) != 0){
		goto out;
	}
	for(i = 1; i < n; i++){
		pp++;
		if(getfields(line[i], field, 3, ' ') != 3){
			break;
		}
		strncpy(pp->name, field[0], NAMELEN);
		pp->start = strtoul(field[1], 0, 0);
		pp->end = strtoul(field[2], 0, 0);
		if(pp->start > pp->end || pp->start >= dp->p[0].end){
			break;
		}
		dp->npart++;
	}
out:
	qunlock(cp);
	poperror();
}

/*
 *  we get an interrupt for every sector transferred
 */
static void
hardintr(Ureg *ur)
{
	Controller *cp;
	Drive *dp;
	long loop;

	spllo();	/* let in other interrupts */

	/*
 	 *  BUG!! if there is ever more than one controller, we need a way to
	 *	  distinguish which interrupted
	 */
	cp = &hardc[0];
	dp = cp->dp;

	loop = 0;
	while((cp->status = inb(cp->pbase+Pstatus)) & Sbusy)
		if(++loop > 10000)
			panic("hardintr 0");
	switch(cp->cmd){
	case Cwrite:
		if(cp->status & Serr){
			cp->cmd = 0;
			cp->error = inb(cp->pbase+Perror);
			wakeup(&cp->r);
			return;
		}
		cp->sofar++;
		if(cp->sofar < cp->nsecs){
			loop = 0;
			while((inb(cp->pbase+Pstatus) & Sdrq) == 0)
				if(++loop > 10000)
					panic("hardintr 1");
			outss(cp->pbase+Pdata, &cp->buf[cp->sofar*dp->bytes],
				dp->bytes/2);
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
		inss(cp->pbase+Pdata, &cp->buf[cp->sofar*dp->bytes],
			dp->bytes/2);
		cp->sofar++;
		if(cp->sofar >= cp->nsecs){
			cp->cmd = 0;
			wakeup(&cp->r);
		}
		break;
	case Csetbuf:
		cp->cmd = 0;
		wakeup(&cp->r);
		break;
	default:
		print("weird disk interrupt\n");
		break;
	}
}
