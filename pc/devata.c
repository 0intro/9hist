/*
 * This has gotten a bit messy with the addition of multiple controller
 * and ATAPI support; needs a rewrite before adding any 'ctl' functions.
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"


#define DPRINT if(0)print
#define XPRINT if(0)print
#define ILOCK(x)
#define IUNLOCK(x)

typedef	struct Drive		Drive;
typedef	struct Ident		Ident;
typedef	struct Controller	Controller;
typedef struct Partition	Partition;
typedef struct Repl		Repl;

enum
{
	/* ports */
	Pbase0=		0x1F0,	/* primary */
	Pbase1=		0x170,	/* secondary */
	Pbase2=		0x1E8,	/* tertiary */
	Pbase3=		0x168,	/* quaternary */
	Pdata=		0,	/* data port (16 bits) */
	Perror=		1,	/* error port (read) */
	 Eabort=	(1<<2),
	Pfeature=	1,	/* buffer mode port (write) */
	Pcount=		2,	/* sector count port */
	Psector=	3,	/* sector number port */
	Pcyllsb=	4,	/* least significant byte cylinder # */
	Pcylmsb=	5,	/* most significant byte cylinder # */
	Pdh=		6,	/* drive/head port */
	 DHmagic=	0xA0,
	 DHslave=	0x10,
	Pstatus=	7,	/* status port (read) */
	 Sbusy=		 (1<<7),
	 Sready=	 (1<<6),
	 Sdf=		 (1<<5),
	 Sdrq=		 (1<<3),
	 Serr=		 (1<<0),
	Pcmd=		7,	/* cmd port (write) */

	Pctrl=		0x206,	/* device control, alternate status */
	 nIEN=		(1<<1),
	 Srst=		(1<<2),

	/* commands */
	Cfirst=		0xFF,	/* pseudo command for initialisation */
	Cread=		0x20,
	Cwrite=		0x30,
	Cedd=		0x90,	/* execute device diagnostics */
	Cident=		0xEC,
	Cident2=	0xFE,	/* pseudo command for post Cident interrupt */
	Cfeature=	0xEF,

	Cstandby=	0xE2,

	Cpktcmd=	0xA0,
	Cidentd=	0xA1,
	Ctur=		0x00,
	Creqsense=	0x03,
	Ccapacity=	0x25,
	Cread2=		0x28,

	/* disk states */
	Sspinning,
	Sstandby,

	/* file types */
	Qdir=		0,

	Maxxfer=	BY2PG,		/* maximum transfer size/cmd */
	Npart=		20+2,		/* 8 sub partitions, disk, and partition */
	Nrepl=		64,		/* maximum replacement blocks */

	Hardtimeout=	6000,		/* disk access timeout (ms) */

	NCtlr=		4,
	NDrive=		NCtlr*2,
};

#define PART(x)		((x)&0xF)
#define DRIVE(x)	(((x)>>4)&0x7)
#define MKQID(d,p)	(((d)<<4) | (p))

struct Partition
{
	ulong	start;
	ulong	end;
	char	name[NAMELEN+1];
};

struct Repl
{
	Partition *p;
	int	nrepl;
	ulong	blk[Nrepl];
};

#define PARTMAGIC	"plan9 partitions"
#define REPLMAGIC	"block replacements"

/*
 *  an ata drive
 */
struct Drive
{
	QLock;

	Controller *cp;
	uchar	driveno;
	uchar	dh;		/* DHmagic|Am-I-A-Slave */
	uchar	atapi;
	uchar	online;

	int	npart;		/* number of real partitions */
	int	partok;
	Partition p[Npart];
	Repl	repl;
	ulong	usetime;
	int	state;
	char	vol[NAMELEN];

	ulong	cap;		/* total bytes */
	int	bytes;		/* bytes/sector */
	int	sectors;	/* sectors/track */
	int	heads;		/* heads/cyl */
	long	cyl;		/* cylinders/drive */
	ulong	lbasecs;

	uchar	lba;		/* true if drive has logical block addressing */
	uchar	multi;		/* true if drive can do multiple block xfers (unused) */
	uchar	drqintr;	/* ATAPI */
	ulong	vers;		/* ATAPI */

	int	spindown;
};

/*
 *  a controller for 2 drives
 */
struct Controller
{
	QLock*	ctlrlock;	/* exclusive access to the controller */

	Lock	reglock;	/* exclusive access to the registers */

	int	pbase;		/* base port */
	uchar	ctlrno;
	uchar	resetok;

	/*
	 *  current operation
	 */
	Rendez	r;		/* wait here for command termination */
	uchar	cmd;		/* current command */
	uchar	cmdblk[12];	/* ATAPI */
	int	len;		/* ATAPI */
	int	count;		/* ATAPI */
	uchar	lastcmd;	/* debugging info */
	uchar	status;
	uchar	error;
	uchar*	buf;		/* xfer buffer */
	int	nsecs;		/* length of transfer (sectors) */
	int	sofar;		/* sectors transferred so far */
	Drive*	dp;		/* drive being accessed */
};

static QLock ataprobelock;
static int ataprobedone;
static Controller *atactlr[NCtlr];
static QLock atactlrlock[NCtlr];
static Drive *atadrive[NDrive];
static int spindownmask;
static int have640b = -1;
static int pbase[NCtlr] = {
	Pbase0, Pbase1, Pbase2, Pbase3,
};
static int defirq[NCtlr] = {
	14, 15, 0, 0,
};

static void	ataintr(Ureg*, void*);
static long	ataxfer(Drive*, Partition*, int, long, long, uchar*);
static void	ataident(Drive*);
static void	atafeature(Drive*, uchar);
static void	ataparams(Drive*);
static void	atapart(Drive*);
static int	ataprobe(Drive*, int, int, int);
static void	atasleep(Controller*, int);

static int	isatapi(Drive*);
static long	atapirwio(Chan*, char*, ulong, ulong);
static void	atapipart(Drive*);
static void	atapiintr(Controller*);

static int
atagen(Chan *c, Dirtab*, long, long s, Dir *dirp)
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

	if(drive >= NDrive)
		return -1;
	if(atadrive[drive] == 0)
		return 0;
	dp = atadrive[drive];

	if(dp->online == 0 || s >= dp->npart)
		return 0;

	pp = &dp->p[s];
	sprint(name, "%s%s", dp->vol, pp->name);
	name[NAMELEN] = 0;
	qid.path = MKQID(drive, s);
	l = (pp->end - pp->start) * dp->bytes;
	devdir(c, qid, name, l, eve, 0660, dirp);
	return 1;
}

static void
cmd640b(void)
{
	PCIcfg* pcicfg;
	uchar r50[12];
	int devno;
	extern void pcicfgw8(int, int, int, int, void*, int);

	/*
	 * Look for CMD640B dual PCI controllers. Amongst other
	 * bugs only one of the controllers can be active at a time.
	 * Unfortunately there's no way to tell which pair of
	 * controllers this is, so if one is found then all controller
	 * pairs are synchronised.
	 */
	pcicfg = malloc(sizeof(PCIcfg));
	pcicfg->vid = 0x1095;
	pcicfg->did = 0x0640;
	devno = 0;
	while((devno = pcimatch(0, devno, pcicfg)) != -1){
		have640b = devno-1;
		/*
		 * If one is found, make sure read-ahead is disabled on all
		 * drives and that the 2nd controller is enabled:
		 *   reg 0x51:	bit 7 - drive 1 read ahead disable
		 *  		bit 6 - drive 0 read ahead disable
		 *  		bit 3 - 2nd controller enable
		 *   reg 0x57:	bit 3 - drive 1 read ahead disable
		 *  		bit 2 - drive 0 read ahead disable
		 * Doing byte-writes to PCI configuration space is not in the
		 * spec...
		 */
		pcicfgr(0, have640b, 0, 0x50, r50, sizeof(r50));
		r50[0x01] |= 0xC8;
		pcicfgw8(0, have640b, 0, 0x51, &r50[0x01], sizeof(r50[0x01]));
		r50[0x07] |= 0x0C;
		pcicfgw8(0, have640b, 0, 0x57, &r50[0x07], sizeof(r50[0x07]));
	}
	free(pcicfg);
}

static void
rz1000(void)
{
	PCIcfg* pcicfg;
	ulong r40;
	int devno;

	/*
	 * Look for PC-Tech RZ1000 controllers and turn off prefetch.
	 * This is overkill, but cheap.
	 */
	pcicfg = malloc(sizeof(PCIcfg));
	pcicfg->vid = 0x1042;
	pcicfg->did = 0;
	devno = 0;
	while((devno = pcimatch(0, devno, pcicfg)) != -1){
		if(pcicfg->did != 0x1000 && pcicfg->did != 0x1001)
			continue;
		pcicfgr(0, devno-1, 0, 0x40, &r40, sizeof(r40));
		r40 &= ~0x2000;
		pcicfgw(0, devno-1, 0, 0x40, &r40, sizeof(r40));
	}
	free(pcicfg);
}

static int
atactlrwait(Controller* ctlr, uchar pdh, uchar ready, ulong ticks)
{
	int port;
	uchar dh, status;

	port = ctlr->pbase;
	dh = (inb(port+Pdh) & DHslave)^(pdh & DHslave);
	ticks += m->ticks+1;

	do{
		status = inb(port+Pstatus);
		if(status & Sbusy)
			continue;
		if(dh){
			outb(port+Pdh, pdh);
			dh = 0;
			continue;
		}
		if((status & ready) == ready)
			return 0;
	}while(m->ticks < ticks);

	DPRINT("ata%d: ctlrwait failed %uX\n", ctlr->ctlrno, status);
	outb(port+Pdh, DHmagic);
	return 1;
}

static void
atadrivealloc(Controller* ctlr, int driveno, int atapi)
{
	Drive *drive;

	if((drive = xalloc(sizeof(Drive))) == 0){
		DPRINT("ata%d: can't xalloc drive0\n", ctlr->ctlrno);
		return;
	}
	drive->cp = ctlr;
	drive->driveno = driveno;
	sprint(drive->vol, "hd%d", drive->driveno);
	drive->dh = DHmagic;
	if(driveno & 0x01)
		drive->dh |= DHslave;
	drive->vers = 1;
	if(atapi){
		sprint(drive->vol, "atapi%d", drive->driveno);
		drive->atapi = 1;
	}

	atadrive[driveno] = drive;
}

static int
atactlrprobe(int ctlrno, int irq)
{
	Controller *ctlr;
	int atapi, mask, port;
	uchar error, status, msb, lsb;

	/*
	 * Check the existence of a controller by verifying a sensible
	 * value can be written to and read from the drive/head register.
	 * We define the primary/secondary/tertiary and quaternary controller
	 * port addresses to be at fixed values.
	 * If it's OK, allocate and initialise a Controller structure.
	 */
	port = pbase[ctlrno];
	outb(port+Pdh, DHmagic);
	microdelay(1);
	if((inb(port+Pdh) & 0xFF) != DHmagic){
		DPRINT("ata%d: DHmagic not ok\n", ctlrno);
		return -1;
	}
	DPRINT("ata%d: DHmagic ok\n", ctlrno);
	if((ctlr = xalloc(sizeof(Controller))) == 0)
		return -1;
	ctlr->pbase = port;
	ctlr->ctlrno = ctlrno;
	ctlr->lastcmd = 0xFF;


	/*
	 * Attempt to check the existence of drives on the controller
	 * by issuing a 'check device diagnostics' command.
	 * Issuing a device reset here would possibly destroy any BIOS
	 * drive remapping and, anyway, some controllers (Vibra16) don't
	 * seem to implement the control-block registers; do it if requested.
	 * Unfortunately the vector must be set at this point as the Cedd
	 * command will generate an interrupt, which means the ataintr routine
	 * will be left on the interrupt call chain even if there are no
	 * drives found.
	 * At least one controller/ATAPI-drive combination doesn't respond
	 * to the Cedd (Micronics M54Li + Sanyo CRD-254P) so let's check for the
	 * ATAPI signature straight off. If we find it there will be no probe
	 * done for a slave. Tough.
	 */
	if(ctlr->resetok){
		outb(port+Pctrl, Srst|nIEN);
		delay(10);
		outb(port+Pctrl, 0);
		if(atactlrwait(ctlr, DHmagic, 0, MS2TK(20))){
			DPRINT("ata%d: Srst status %ux/%ux/%ux\n", ctlrno,
				inb(port+Pstatus), inb(port+Pcylmsb), inb(port+Pcyllsb));
			xfree(ctlr);
		}
	}

	atapi = 0;
	mask = 0;
	status = inb(port+Pstatus);
	DPRINT("ata%d: ATAPI %uX %uX %uX\n", ctlrno, status,
		inb(port+Pcylmsb), inb(port+Pcyllsb));
	if(status == 0 && inb(port+Pcylmsb) == 0xEB && inb(port+Pcyllsb) == 0x14){
		DPRINT("ata%d: ATAPI ok\n", ctlrno);
		setvec(irq, ataintr, ctlr);
		atapi |= 0x01;
		mask |= 0x01;
		goto skipedd;
	}
	if(atactlrwait(ctlr, DHmagic, 0, MS2TK(1)) || waserror()){
		DPRINT("ata%d: Cedd status %ux/%ux/%ux\n", ctlrno,
			inb(port+Pstatus), inb(port+Pcylmsb), inb(port+Pcyllsb));
		xfree(ctlr);
		return -1;
	}
	setvec(irq, ataintr, ctlr);
	ctlr->cmd = Cedd;
	outb(port+Pcmd, Cedd);
	atasleep(ctlr, Hardtimeout);
	poperror();

	/*
	 * The diagnostic returns a code in the error register, good
	 * status is bits 6-0 == 0x01.
	 * The existence of the slave is more difficult to determine,
	 * different generations of controllers may respond in different
	 * ways. The standards here offer little light but only more and
	 * more heat:
	 *   1) the slave must be done and have dropped Sbusy by now (six
	 *	seconds for the master, 5 seconds for the slave). If it
	 *	hasn't, then it has either failed or the controller is
	 *	broken in some way (e.g. Vibra16 returns status of 0xFF);
	 *   2) theory says the status of a non-existent slave should be 0.
	 *	Of course, it's valid for all the bits to be 0 for a slave
	 *	that exists too...
	 *   3) a valid ATAPI drive can have status 0 and the ATAPI signature
	 *	in the cylinder registers after reset. Of course, if the drive
	 *	has been messed about by the BIOS or some other O/S then the
	 *	signature may be gone.
	 */
	error = inb(port+Perror);
	DPRINT("ata%d: master diag error %ux\n", ctlr->ctlrno, error);
	if((error & ~0x80) == 0x01)
		mask |= 0x01;

	outb(port+Pdh, DHmagic|DHslave);
	microdelay(1);
	status = inb(port+Pstatus);
	error = inb(port+Perror);
	DPRINT("ata%d: slave diag status %ux, error %ux\n", ctlr->ctlrno, status, error);
	if(status && (status & (Sbusy|Serr)) == 0 && (error & ~0x80) == 0x01)
		mask |= 0x02;
	else if(status == 0){
		msb = inb(port+Pcylmsb);
		lsb = inb(port+Pcyllsb);
		DPRINT("ata%d: ATAPI slave %uX %uX %uX\n", ctlrno, status,
			inb(port+Pcylmsb), inb(port+Pcyllsb));
		if(msb == 0xEB && lsb == 0x14){
			atapi |= 0x02;
			mask |= 0x02;
		}
	}

skipedd:
	if(mask == 0){
		xfree(ctlr);
		return -1;
	}
	atactlr[ctlrno] = ctlr;

	if(have640b >= 0 && (ctlrno & 0x01))
		ctlr->ctlrlock = &atactlrlock[ctlrno-1];
	else
		ctlr->ctlrlock = &atactlrlock[ctlrno];

	if(mask & 0x01)
		atadrivealloc(ctlr, ctlrno*2, atapi & 0x01);
	if(mask & 0x02)
		atadrivealloc(ctlr, ctlrno*2+1, atapi & 0x02);

	return 0;
}

void
atactlrreset(void)
{
	int ctlrno, driveno, i, slave, spindown;
	ISAConf isa;

	cmd640b();
	rz1000();

	for(ctlrno = 0; ctlrno < NCtlr; ctlrno++){
		memset(&isa, 0, sizeof(ISAConf));
		if(isaconfig("ata", ctlrno, &isa) == 0 && ctlrno)
			continue;
		if(isa.irq == 0 && (isa.irq = defirq[ctlrno]) == 0)
			continue;

		if(atactlrprobe(ctlrno, Int0vec+isa.irq))
			continue;
	
		for(i = 0; i < isa.nopt; i++){
			DPRINT("ata%d: opt %s\n", ctlrno, isa.opt[i]);
			if(strncmp(isa.opt[i], "spindown", 8) == 0){
				if(isa.opt[i][9] != '=')
					continue;
				if(isa.opt[i][8] == '0')
					slave = 0;
				else if(isa.opt[i][8] == '1')
					slave = 1;
				else
					continue;
				driveno = ctlrno*2+slave;
				if(atadrive[driveno] == 0)
					continue;
				if((spindown = strtol(&isa.opt[i][10], 0, 0)) == 0)
					continue;
				if(spindown < (Hardtimeout+2000)/1000)
					spindown = (Hardtimeout+2000)/1000;
				atadrive[driveno]->spindown = spindown;
				spindownmask |= (1<<driveno);
				DPRINT("ata%d: opt spindownmask %ux\n",
					ctlrno, spindownmask);
			}
			else if(strcmp(isa.opt[i], "reset") == 0)
				atactlr[ctlrno]->resetok = 1;
		}
	}
}

void
atareset(void)
{
}

void
atainit(void)
{
}

/*
 *  Get the characteristics of each drive.  Mark unresponsive ones
 *  off line.
 */
Chan*
ataattach(char *spec)
{
	int driveno;
	Drive *dp;

	DPRINT("ataattach\n");

	qlock(&ataprobelock);
	if(ataprobedone == 0){
		atactlrreset();
		ataprobedone = 1;
	}
	qunlock(&ataprobelock);

	for(driveno = 0; driveno < NDrive; driveno++){
		if((dp = atadrive[driveno]) == 0)
			continue;
		if(waserror()){
			dp->online = 0;
			qunlock(dp);
			continue;
		}
		qlock(dp);
		if(!dp->online){
			ataparams(dp);
			dp->online = 1;
			atafeature(dp, 0xAA);	/* read look ahead */
		}

		/*
		 *  read Plan 9 partition table
		 */
		if(dp->partok == 0){
			if(dp->atapi)
				atapipart(dp);
			else
				atapart(dp);
		}
		qunlock(dp);
		poperror();
	}
	return devattach('H', spec);
}

Chan*
ataclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
atawalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, atagen);
}

void
atastat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, atagen);
}

Chan*
ataopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, atagen);
}

void
atacreate(Chan*, char*, int, ulong)
{
	error(Eperm);
}

void
ataclose(Chan *c)
{
	Drive *dp;
	Partition *p;

	if(c->mode != OWRITE && c->mode != ORDWR)
		return;

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp == 0)
		return;
	p = &dp->p[PART(c->qid.path)];
	if(strcmp(p->name, "partition") != 0)
		return;

	if(waserror()){
		qunlock(dp);
		nexterror();
	}
	qlock(dp);
	dp->partok = 0;
	atapart(dp);
	qunlock(dp);
	poperror();
}

void
ataremove(Chan*)
{
	error(Eperm);
}

void
atawstat(Chan*, char*)
{
	error(Eperm);
}

long
ataread(Chan *c, void *a, long n, ulong offset)
{
	Drive *dp;
	long rv, i;
	int skip;
	uchar *aa = a;
	Partition *pp;
	uchar *buf;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, 0, 0, atagen);

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp->atapi){
		if(dp->online == 0)
			error(Eio);
		if(waserror()){
			qunlock(dp);
			nexterror();
		}
		qlock(dp);
		if(dp->partok == 0)
			atapipart(dp);
		qunlock(dp);
		poperror();
		return atapirwio(c, a, n, offset);
	}
	pp = &dp->p[PART(c->qid.path)];

	buf = smalloc(Maxxfer);
	if(waserror()){
		free(buf);
		nexterror();
	}

	skip = offset % dp->bytes;
	for(rv = 0; rv < n; rv += i){
		i = ataxfer(dp, pp, Cread, offset+rv-skip, n-rv+skip, buf);
		if(i == 0)
			break;
		i -= skip;
		if(i > n - rv)
			i = n - rv;
		memmove(aa+rv, buf + skip, i);
		skip = 0;
	}

	free(buf);
	poperror();

	return rv;
}

Block*
atabread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
atawrite(Chan *c, void *a, long n, ulong offset)
{
	Drive *dp;
	long rv, i, partial;
	uchar *aa = a;
	Partition *pp;
	uchar *buf;

	if(c->qid.path == CHDIR)
		error(Eisdir);

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp->atapi)
		error(Eperm);
	pp = &dp->p[PART(c->qid.path)];

	buf = smalloc(Maxxfer);
	if(waserror()){
		free(buf);
		nexterror();
	}

	/*
	 *  if not starting on a sector boundary,
	 *  read in the first sector before writing
	 *  it out.
	 */
	partial = offset % dp->bytes;
	if(partial){
		ataxfer(dp, pp, Cread, offset-partial, dp->bytes, buf);
		if(partial+n > dp->bytes)
			rv = dp->bytes - partial;
		else
			rv = n;
		memmove(buf+partial, aa, rv);
		ataxfer(dp, pp, Cwrite, offset-partial, dp->bytes, buf);
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
		memmove(buf, aa+rv, i);
		i = ataxfer(dp, pp, Cwrite, offset+rv, i, buf);
		if(i == 0)
			break;
	}

	/*
	 *  if not ending on a sector boundary,
	 *  read in the last sector before writing
	 *  it out.
	 */
	if(partial){
		ataxfer(dp, pp, Cread, offset+rv, dp->bytes, buf);
		memmove(buf, aa+rv, partial);
		ataxfer(dp, pp, Cwrite, offset+rv, dp->bytes, buf);
		rv += partial;
	}

	free(buf);
	poperror();

	return rv;
}

long
atabwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
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
 * Wait for the controller to be ready to accept a command.
 */
static int
cmdreadywait(Drive *dp)
{
	ulong ticks;
	uchar ready;

	/* give it 2 seconds to spin down and up */
	dp->usetime = m->ticks;
	if(dp->state == Sspinning)
		ticks = MS2TK(10);
	else
		ticks = MS2TK(2000);

	if(dp->atapi)
		ready = 0;
	else
		ready = Sready;

	return atactlrwait(dp->cp, dp->dh, ready, ticks);
}

static void
atarepl(Drive *dp, long bblk)
{
	int i;

	if(dp->repl.p == 0)
		return;
	for(i = 0; i < dp->repl.nrepl; i++){
		if(dp->repl.blk[i] == bblk)
			DPRINT("%s: found bblk %ld at offset %ld\n", dp->vol, bblk, i);
	}
}

static void
atasleep(Controller *cp, int ms)
{
	tsleep(&cp->r, cmddone, cp, ms);
	if(cp->cmd && cp->cmd != Cident2){
		DPRINT("ata%d: cmd 0x%uX timeout\n", cp->ctlrno, cp->cmd);
		error("ata drive timeout");
	}
}


/*
 *  transfer a number of sectors.  ataintr will perform all the iterative
 *  parts.
 */
static long
ataxfer(Drive *dp, Partition *pp, int cmd, long start, long len, uchar *buf)
{
	Controller *cp;
	long lblk;
	int cyl, sec, head;
	int loop, stat;

	if(dp->online == 0)
		error(Eio);

	/*
	 *  cut transfer size down to disk buffer size
	 */
	start = start / dp->bytes;
	if(len > Maxxfer)
		len = Maxxfer;
	len = (len + dp->bytes - 1) / dp->bytes;
	if(len == 0)
		return 0;

	/*
	 *  calculate physical address
	 */
	lblk = start + pp->start;
	if(lblk >= pp->end)
		return 0;
	if(lblk+len > pp->end)
		len = pp->end - lblk;
	if(dp->lba){
		sec = lblk & 0xff;
		cyl = (lblk>>8) & 0xffff;
		head = (lblk>>24) & 0xf;
	} else {
		cyl = lblk/(dp->sectors*dp->heads);
		sec = (lblk % dp->sectors) + 1;
		head = ((lblk/dp->sectors) % dp->heads);
	}

	XPRINT("%s: ataxfer cyl %d sec %d head %d len %d\n", dp->vol, cyl, sec, head, len);

	cp = dp->cp;
	qlock(cp->ctlrlock);
	if(waserror()){
		cp->dp = 0;
		cp->buf = 0;
		qunlock(cp->ctlrlock);
		nexterror();
	}

	if(cmdreadywait(dp)){
		error(Eio);
	}

	ILOCK(&cp->reglock);
	cp->sofar = 0;
	cp->buf = buf;
	cp->nsecs = len;
	cp->cmd = cmd;
	cp->dp = dp;
	cp->status = 0;

	outb(cp->pbase+Pcount, cp->nsecs);
	outb(cp->pbase+Psector, sec);
	outb(cp->pbase+Pdh, dp->dh | (dp->lba<<6) | head);
	outb(cp->pbase+Pcyllsb, cyl);
	outb(cp->pbase+Pcylmsb, cyl>>8);
	outb(cp->pbase+Pcmd, cmd);

	if(cmd == Cwrite){
		loop = 0;
		microdelay(1);
		while((stat = inb(cp->pbase+Pstatus) & (Serr|Sdrq)) == 0)
			if(++loop > 10000)
				panic("%s: ataxfer", dp->vol);
		outss(cp->pbase+Pdata, cp->buf, dp->bytes/2);
	} else
		stat = 0;
	IUNLOCK(&cp->reglock);

	if(stat & Serr)
		error(Eio);

	/*
	 *  wait for command to complete.  if we get a note,
	 *  remember it but keep waiting to let the disk finish
	 *  the current command.
	 */
	loop = 0;
	while(waserror()){
		DPRINT("%s: interrupted ataxfer\n", dp->vol);
		if(loop++ > 10){
			print("%s: disk error\n", dp->vol);
			nexterror();
		}
	}
	atasleep(cp, Hardtimeout);
	dp->usetime = m->ticks;
	dp->state = Sspinning;
	poperror();
	if(loop)
		nexterror();

	if(cp->status & Serr){
		DPRINT("%s err: lblk %ld status %lux, err %lux\n",
			dp->vol, lblk, cp->status, cp->error);
		DPRINT("\tcyl %d, sec %d, head %d\n", cyl, sec, head);
		DPRINT("\tnsecs %d, sofar %d\n", cp->nsecs, cp->sofar);
		atarepl(dp, lblk+cp->sofar);
		error(Eio);
	}
	cp->dp = 0;
	cp->buf = 0;
	len = cp->sofar*dp->bytes;
	qunlock(cp->ctlrlock);
	poperror();

	return len;
}

/*
 *  set read ahead mode
 */
static void
atafeature(Drive *dp, uchar arg)
{
	Controller *cp = dp->cp;

	if(dp->atapi)
		return;

	qlock(cp->ctlrlock);
	if(waserror()){
		cp->dp = 0;
		qunlock(cp->ctlrlock);
		nexterror();
	}

	if(cmdreadywait(dp)){
		error(Eio);
	}

	ILOCK(&cp->reglock);
	cp->cmd = Cfeature;
	cp->dp = dp;
	outb(cp->pbase+Pfeature, arg);
	outb(cp->pbase+Pdh, dp->dh);
	outb(cp->pbase+Pcmd, Cfeature);
	IUNLOCK(&cp->reglock);

	atasleep(cp, Hardtimeout);

	if(cp->status & Serr)
		DPRINT("%s: setbuf err: status %lux, err %lux\n",
			dp->vol, cp->status, cp->error);

	cp->dp = 0;
	poperror();
	qunlock(cp->ctlrlock);
}

/*
 *  ident sector from drive.  this is from ANSI X3.221-1994
 */
struct Ident
{
	ushort	config;		/* general configuration info */
	ushort	cyls;		/* # of cylinders (default) */
	ushort	reserved0;
	ushort	heads;		/* # of heads (default) */
	ushort	b2t;		/* unformatted bytes/track */
	ushort	b2s;		/* unformated bytes/sector */
	ushort	s2t;		/* sectors/track (default) */
	ushort	reserved1[3];
/* 10 */
	ushort	serial[10];	/* serial number */
	ushort	type;		/* buffer type */
	ushort	bsize;		/* buffer size/512 */
	ushort	ecc;		/* ecc bytes returned by read long */
	ushort	firm[4];	/* firmware revision */
	ushort	model[20];	/* model number */
/* 47 */
	ushort	s2i;		/* number of sectors/interrupt */
	ushort	dwtf;		/* double word transfer flag */
	ushort	capabilities;
	ushort	reserved2;
	ushort	piomode;
	ushort	dmamode;
	ushort	cvalid;		/* (cvald&1) if next 4 words are valid */
	ushort	ccyls;		/* current # cylinders */
	ushort	cheads;		/* current # heads */
	ushort	cs2t;		/* current sectors/track */
	ushort	ccap[2];	/* current capacity in sectors */
	ushort	cs2i;		/* current number of sectors/interrupt */
/* 60 */
	ushort	lbasecs[2];	/* # LBA user addressable sectors */
	ushort	dmasingle;
	ushort	dmadouble;
/* 64 */
	ushort	reserved3[64];
	ushort	vendor[32];	/* vendor specific */
	ushort	reserved4[96];
};

/*
 *  get parameters from the drive
 */
static void
ataident(Drive *dp)
{
	Controller *cp;
	uchar *buf;
	Ident *ip;
	char id[21];
	ulong lbasecs;
	uchar cmd;

	cp = dp->cp;
	buf = smalloc(Maxxfer);
	qlock(cp->ctlrlock);
	if(waserror()){
		cp->dp = 0;
		free(buf);
		qunlock(cp->ctlrlock);
		nexterror();
	}

	if(dp->atapi)
		cmd = Cidentd;
	else
		cmd = Cident;
retryatapi:
	ILOCK(&cp->reglock);
	cp->nsecs = 1;
	cp->sofar = 0;
	cp->cmd = cmd;
	cp->dp = dp;
	cp->buf = buf;
	outb(cp->pbase+Pdh, dp->dh);
	outb(cp->pbase+Pcmd, cmd);
	IUNLOCK(&cp->reglock);

	DPRINT("%s: ident command %ux sent\n", dp->vol, cmd);
	if(cmd == Cident)
		atasleep(cp, 3000);
	else
		atasleep(cp, 10000);

	if(cp->status & Serr){
		DPRINT("%s: bad disk ident status\n", dp->vol);
		if(cp->error & Eabort){
			if(isatapi(dp)){
				cmd = Cidentd;
				goto retryatapi;
			}
		}
		error(Eio);
	}
	ip = (Ident*)buf;

	/*
	 * this function appears to respond with an extra interrupt after
	 * the ident information is read, except on the safari.  The following
	 * delay gives this extra interrupt a chance to happen while we are quiet.
	 * Otherwise, the interrupt may come during a subsequent read or write,
	 * causing a panic and much confusion.
	 */
	if (cp->cmd == Cident2)
		tsleep(&cp->r, return0, 0, Hardtimeout);

	memmove(id, ip->model, sizeof(id)-1);
	id[sizeof(id)-1] = 0;

	DPRINT("%s: config 0x%uX capabilities 0x%uX\n",
		dp->vol, ip->config, ip->capabilities);
	if(dp->atapi){
		dp->bytes = 2048;
		if((ip->config & 0x0060) == 0x0020)
			dp->drqintr = 1;
	}
	if(dp->spindown && (ip->capabilities & (1<<13)))
		dp->spindown /= 5;

	/* use default (unformatted) settings */
	dp->cyl = ip->cyls;
	dp->heads = ip->heads;
	dp->sectors = ip->s2t;
	XPRINT("%s: %s %d/%d/%d CHS %d bytes\n",
		dp->vol, id, dp->cyl, dp->heads, dp->sectors, dp->cap);

	if(ip->cvalid&(1<<0)){
		/* use current settings */
		dp->cyl = ip->ccyls;
		dp->heads = ip->cheads;
		dp->sectors = ip->cs2t;
		XPRINT("%s: changed to %d cyl %d head %d sec\n",
			dp->vol, dp->cyl, dp->heads, dp->sectors);
	}

	lbasecs = (ip->lbasecs[0]) | (ip->lbasecs[1]<<16);
	if((ip->capabilities & (1<<9)) && (lbasecs & 0xf0000000) == 0){
		dp->lba = 1;
		dp->lbasecs = lbasecs;
		dp->cap = dp->bytes * dp->lbasecs;
		XPRINT("%s: LBA: %s %d sectors %d bytes\n",
			dp->vol, id, dp->lbasecs, dp->cap);
	} else {
		dp->lba = 0;
		dp->lbasecs = 0;
		dp->cap = dp->bytes * dp->cyl * dp->heads * dp->sectors;
	}

	if(cp->cmd){
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
	}
	cp->dp = 0;
	cp->buf = 0;
	free(buf);
	poperror();
	qunlock(cp->ctlrlock);
}

/*
 *  probe the given sector to see if it exists
 */
static int
ataprobe(Drive *dp, int cyl, int sec, int head)
{
	Controller *cp;
	uchar *buf;
	int rv;

	cp = dp->cp;
	buf = smalloc(Maxxfer);
	qlock(cp->ctlrlock);
	if(waserror()){
		cp->dp = 0;
		free(buf);
		qunlock(cp->ctlrlock);
		nexterror();
	}

	if(cmdreadywait(dp)){
		error(Eio);
	}

	ILOCK(&cp->reglock);
	cp->cmd = Cread;
	cp->dp = dp;
	cp->status = 0;
	cp->nsecs = 1;
	cp->sofar = 0;
	cp->buf = buf;

	outb(cp->pbase+Pcount, 1);
	outb(cp->pbase+Psector, sec+1);
	outb(cp->pbase+Pdh, dp->dh | (dp->lba<<6) | head);
	outb(cp->pbase+Pcyllsb, cyl);
	outb(cp->pbase+Pcylmsb, cyl>>8);
	outb(cp->pbase+Pcmd, Cread);
	IUNLOCK(&cp->reglock);

	atasleep(cp, Hardtimeout);

	if(cp->status & Serr){
		DPRINT("%s: probe err: status %lux, err %lux\n",
			dp->vol, cp->status, cp->error);
		rv = -1;
	}
	else
		rv = 0;

	cp->dp = 0;
	cp->buf = 0;
	free(buf);
	poperror();
	qunlock(cp->ctlrlock);
	return rv;
}

/*
 *  figure out the drive parameters
 */
static void
ataparams(Drive *dp)
{
	int i, hi, lo;

	/*
	 *  first try the easy way, ask the drive and make sure it
	 *  isn't lying.
	 */
	dp->bytes = 512;
	ataident(dp);
	if(dp->atapi)
		return;
	if(dp->lba){
		i = dp->lbasecs - 1;
		if(ataprobe(dp, (i>>8)&0xffff, (i&0xff)-1, (i>>24)&0xf) == 0)
			return;
	} else {
		if(ataprobe(dp, dp->cyl-1, dp->sectors-1, dp->heads-1) == 0)
			return;
	}

	/*
	 *  the drive lied, determine parameters by seeing which ones
	 *  work to read sectors.
	 */
	dp->lba = 0;
	for(i = 0; i < 16; i++)
		if(ataprobe(dp, 0, 0, i) < 0)
			break;
	dp->heads = i;
	for(i = 0; i < 64; i++)
		if(ataprobe(dp, 0, i, 0) < 0)
			break;
	dp->sectors = i;
	for(i = 512; ; i += 512)
		if(ataprobe(dp, i, dp->sectors-1, dp->heads-1) < 0)
			break;
	lo = i - 512;
	hi = i;
	for(; hi-lo > 1;){
		i = lo + (hi - lo)/2;
		if(ataprobe(dp, i, dp->sectors-1, dp->heads-1) < 0)
			hi = i;
		else
			lo = i;
	}
	dp->cyl = lo + 1;
	dp->cap = dp->bytes * dp->cyl * dp->heads * dp->sectors;
	DPRINT("%s: probed: %d/%d/%d CHS %d bytes\n",
		dp->vol, dp->cyl, dp->heads, dp->sectors, dp->cap);
	if(dp->cyl == 0 || dp->heads == 0 || dp->sectors == 0)
		error(Eio);
}

/*
 *  Read block replacement table.
 *  The table is just ascii block numbers.
 */
static void
atareplinit(Drive *dp)
{
	char *line[Nrepl+1];
	char *field[1];
	ulong n;
	int i;
	uchar *buf;

	/*
	 *  check the partition is big enough
	 */
	if(dp->repl.p->end - dp->repl.p->start < Nrepl+1){
		dp->repl.p = 0;
		return;
	}

	buf = smalloc(Maxxfer);
	if(waserror()){
		free(buf);
		nexterror();
	}

	/*
	 *  read replacement table from disk, null terminate
	 */
	ataxfer(dp, dp->repl.p, Cread, 0, dp->bytes, buf);
	buf[dp->bytes-1] = 0;

	/*
	 *  parse replacement table.
	 */
	n = parsefields((char*)buf, line, Nrepl+1, "\n");
	if(strncmp(line[0], REPLMAGIC, sizeof(REPLMAGIC)-1)){
		dp->repl.p = 0;
	} else {
		for(dp->repl.nrepl = 0, i = 1; i < n; i++, dp->repl.nrepl++){
			if(parsefields(line[i], field, 1, " ") != 1)
				break;
			dp->repl.blk[dp->repl.nrepl] = strtoul(field[0], 0, 0);
			if(dp->repl.blk[dp->repl.nrepl] <= 0)
				break;
		}
	}
	free(buf);
	poperror();
}

/*
 *  read partition table.  The partition table is just ascii strings.
 */
static void
atapart(Drive *dp)
{
	Partition *pp;
	char *line[Npart+1];
	char *field[3], namebuf[NAMELEN], *p;
	ulong n;
	int i;
	uchar *buf;

	DPRINT("%s: partok %d\n", dp->vol, dp->partok);

	if(dp->partok)
		return;

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
	pp++;
	dp->npart = 2;

	/*
	 * initialise the bad-block replacement info
	 */
	dp->repl.p = 0;

	buf = smalloc(Maxxfer);
	if(waserror()){
		DPRINT("%s: atapart error\n", dp->vol);
		free(buf);
		nexterror();
	}


	/*
	 * Check if the partitions are described in plan9.ini.
	 * If not, read the disc.
	 */
	sprint(namebuf, "%spartition", dp->vol);
	if((p = getconf(namebuf)) == 0){	
		/*
		 *  Read second last sector from disk, null terminate.
		 *  The last sector used to hold the partition tables.
		 *  However, this sector is special on some PC's so we've
		 *  started to use the second last sector as the partition
		 *  table instead.  To avoid reconfiguring all our old systems
		 *  we still check if there is a valid partition table in
		 *  the last sector if none is found in the second last.
		 */
		i = 0;
		ataxfer(dp, &dp->p[0], Cread, (dp->p[0].end-2)*dp->bytes, dp->bytes, buf);
		buf[dp->bytes-1] = 0;
		n = parsefields((char*)buf, line, Npart+1, "\n");
		if(n > 0 && strncmp(line[0], PARTMAGIC, sizeof(PARTMAGIC)-1) == 0)
			i = 1;
		else{
			ataxfer(dp, &dp->p[1], Cread, 0, dp->bytes, buf);
			buf[dp->bytes-1] = 0;
			n = parsefields((char*)buf, line, Npart+1, "\n");
			if(n == 0 || strncmp(line[0], PARTMAGIC, sizeof(PARTMAGIC)-1))
				i = 1;
		}
		if(i){
			dp->p[0].end--;
			dp->p[1].start--;
			dp->p[1].end--;
		}
	}
	else{
		strcpy((char*)buf, p);
		n = parsefields((char*)buf, line, Npart+1, "\n");
	}

	/*
	 *  parse partition table.
	 */
	if(n > 0 && strncmp(line[0], PARTMAGIC, sizeof(PARTMAGIC)-1) == 0){
		for(i = 1; i < n; i++){
			if(parsefields(line[i], field, 3, " ") != 3)
				break;
			if(pp >= &dp->p[Npart])
				break;
			strncpy(pp->name, field[0], NAMELEN);
			if(strncmp(pp->name, "repl", NAMELEN) == 0)
				dp->repl.p = pp;
			pp->start = strtoul(field[1], 0, 0);
			pp->end = strtoul(field[2], 0, 0);
			if(pp->start > pp->end || pp->start >= dp->p[0].end)
				break;
			pp++;
		}
	}
	dp->npart = pp - dp->p;
	free(buf);
	poperror();

	dp->partok = 1;

	if(dp->repl.p)
		atareplinit(dp);
}

enum
{
	Maxloop=	1000000,
};

/*
 *  we get an interrupt for every sector transferred
 */
static void
ataintr(Ureg*, void* arg)
{
	Controller *cp;
	Drive *dp;
	int loop;
	uchar *addr;

	cp = arg;
	if((dp = cp->dp) == 0 && cp->cmd != Cedd)
		return;

	ILOCK(&cp->reglock);

	loop = 0;
	while((cp->status = inb(cp->pbase+Pstatus)) & Sbusy){
		if(++loop > Maxloop){
			print("ata%d: cmd=%lux, lastcmd=%lux status=%lux\n",
				cp->ctlrno, cp->cmd, cp->lastcmd, inb(cp->pbase+Pstatus));
			panic("%s: wait busy\n", dp->vol);
		}
	}

	switch(cp->cmd){
	case Cwrite:
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cp->pbase+Perror);
			wakeup(&cp->r);
			break;
		}
		cp->sofar++;
		if(cp->sofar < cp->nsecs){
			loop = 0;
			while(((cp->status = inb(cp->pbase+Pstatus)) & Sdrq) == 0)
				if(++loop > Maxloop)
					panic("%s: write cmd=%lux status=%lux\n",
						dp->vol, cp->cmd, inb(cp->pbase+Pstatus));
			addr = cp->buf;
			if(addr){
				addr += cp->sofar*dp->bytes;
				outss(cp->pbase+Pdata, addr, dp->bytes/2);
			}
		} else{
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			wakeup(&cp->r);
		}
		break;
	case Cread:
	case Cident:
	case Cidentd:
		loop = 0;
		while((cp->status & (Serr|Sdrq)) == 0){
			if(++loop > Maxloop){
				print("%s: read/ident cmd=%lux status=%lux\n",
					dp->vol, cp->cmd, inb(cp->pbase+Pstatus));
				cp->status |= Serr;
				break;
			}
			cp->status = inb(cp->pbase+Pstatus);
		}
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cp->pbase+Perror);
			wakeup(&cp->r);
			break;
		}
		addr = cp->buf;
		if(addr){
			addr += cp->sofar*dp->bytes;
			inss(cp->pbase+Pdata, addr, dp->bytes/2);
		}
		cp->sofar++;
		if(cp->sofar > cp->nsecs)
			print("%s: intr %d %d\n", dp->vol, cp->sofar, cp->nsecs);
		if(cp->sofar >= cp->nsecs){
			cp->lastcmd = cp->cmd;
			if(cp->cmd != Cread)
				cp->cmd = Cident2;
			else
				cp->cmd = 0;
			inb(cp->pbase+Pstatus);
			wakeup(&cp->r);
		}
		break;
	case Cfeature:
	case Cstandby:
	case Cedd:
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
		wakeup(&cp->r);
		break;
	case Cident2:
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
		break;
	case Cpktcmd:
		atapiintr(cp);
		break;
	default:
		if(cp->cmd == 0 && cp->lastcmd == Cpktcmd && cp->cmdblk[0] == Ccapacity)
			break;
		if(cp->status & Serr)
			cp->error = inb(cp->pbase+Perror);
		print("%s: weird interrupt, cmd=%.2ux, lastcmd=%.2ux, ",
			dp->vol, cp->cmd, cp->lastcmd);
		print("status=%.2ux, error=%.2ux, count=%.2ux\n",
			cp->ctlrno, cp->error, inb(cp->pbase+Pcount));
		break;
	}

	IUNLOCK(&cp->reglock);
}

void
hardclock(void)
{
	int driveno, mask;
	Drive *dp;
	Controller *cp;
	int diff;

	if((mask = spindownmask) == 0)
		return;

	for(driveno = 0; driveno < NDrive && mask; driveno++){
		mask &= ~(1<<driveno);
		if((dp = atadrive[driveno]) == 0)
			continue;
		cp = dp->cp;

		diff = TK2SEC(m->ticks - dp->usetime);
		if((dp->state == Sspinning) && (diff >= dp->spindown)){
			DPRINT("%s: spindown\n", dp->vol);
			ILOCK(&cp->reglock);
			cp->cmd = Cstandby;
			outb(cp->pbase+Pcount, 0);
			outb(cp->pbase+Pdh, dp->dh);
			outb(cp->pbase+Pcmd, cp->cmd);
			IUNLOCK(&cp->reglock);
			dp->state = Sstandby;
		}
	}
}

static int
isatapi(Drive *dp)
{
	Controller *cp;

	cp = dp->cp;
	outb(cp->pbase+Pdh, dp->dh);
	DPRINT("%s: isatapi %d\n", dp->vol, dp->atapi);
	outb(cp->pbase+Pcmd, 0x08);
	if(atactlrwait(dp->cp, DHmagic, 0, MS2TK(100))){
		DPRINT("%s: isatapi ctlrwait status %ux\n", dp->vol, inb(cp->pbase+Pstatus));
		return 0;
	}
	dp->atapi = 0;
	dp->bytes = 512;
	microdelay(1);
	if(inb(cp->pbase+Pstatus)){
		DPRINT("%s: isatapi status %ux\n", dp->vol, inb(cp->pbase+Pstatus));
		return 0;
	}
	if(inb(cp->pbase+Pcylmsb) != 0xEB || inb(cp->pbase+Pcyllsb) != 0x14){
		DPRINT("%s: isatapi cyl %ux %ux\n",
			dp->vol, inb(cp->pbase+Pcylmsb), inb(cp->pbase+Pcyllsb));
		return 0;
	}
	dp->atapi = 1;
	sprint(dp->vol, "atapi%d", dp->driveno);
	dp->spindown = 0;
	spindownmask &= ~(1<<dp->driveno);
	return 1;
}

static void
atapiexec(Drive *dp)
{
	Controller *cp;
	int loop;

	cp = dp->cp;

	if(cmdreadywait(dp)){
		error(Eio);
	}
	
	ILOCK(&cp->reglock);
	cp->nsecs = 1;
	cp->sofar = 0;
	cp->error = 0;
	cp->cmd = Cpktcmd;
	outb(cp->pbase+Pcount, 0);
	outb(cp->pbase+Psector, 0);
	outb(cp->pbase+Pfeature, 0);
	outb(cp->pbase+Pcyllsb, cp->len);
	outb(cp->pbase+Pcylmsb, cp->len>>8);
	outb(cp->pbase+Pdh, dp->dh);
	outb(cp->pbase+Pcmd, cp->cmd);

	if(dp->drqintr == 0){
		microdelay(1);
		for(loop = 0; (inb(cp->pbase+Pstatus) & (Serr|Sdrq)) == 0; loop++){
			if(loop < 10000)
				continue;
			panic("%s: cmddrqwait: cmd=%lux status=%lux\n",
				dp->vol, cp->cmd, inb(cp->pbase+Pstatus));
		}
		outss(cp->pbase+Pdata, cp->cmdblk, sizeof(cp->cmdblk)/2);
	}
	IUNLOCK(&cp->reglock);

	loop = 0;
	while(waserror()){
		DPRINT("%s: interrupted atapiexec\n", dp->vol);
		if(loop++ > 10){
			print("%s: disk error\n", dp->vol);
			nexterror();
		}
	}
	atasleep(cp, Hardtimeout);
	poperror();
	if(loop)
		nexterror();

	if(cp->status & Serr){
		DPRINT("%s: Bad packet command %ux, error %ux\n", dp->vol, cp->cmdblk[0], cp->error);
		error(Eio);
	}
}

static long
atapiio(Drive *dp, char *a, ulong len, ulong offset)
{
	ulong bn, n, o, m;
	Controller *cp;
	uchar *buf;
	int retrycount;

	cp = dp->cp;

	buf = smalloc(Maxxfer);
	qlock(cp->ctlrlock);
	retrycount = 2;
retry:
	if(waserror()){
		dp->partok = 0;
		if((cp->status & Serr) && (cp->error & 0xF0) == 0x60){
			dp->vers++;
			if(retrycount){
				retrycount--;
				goto retry;
			}
		}
		cp->dp = 0;
		free(buf);
		qunlock(cp->ctlrlock);
		nexterror();
	}

	cp->buf = buf;
	cp->dp = dp;
	cp->len = dp->bytes;

	n = len;
	while(n > 0){
		bn = offset / dp->bytes;
		if(offset > dp->cap-dp->bytes)
			break;
		o = offset % dp->bytes;
		m = dp->bytes - o;
		if(m > n)
			m = n;
		memset(cp->cmdblk, 0, 12);
		cp->cmdblk[0] = Cread2;
		cp->cmdblk[2] = bn >> 24;
		cp->cmdblk[3] = bn >> 16;
		cp->cmdblk[4] = bn >> 8;
		cp->cmdblk[5] = bn;
		cp->cmdblk[7] = 0;
		cp->cmdblk[8] = 1;
		atapiexec(dp);
		if(cp->count != dp->bytes){
			print("short read\n");
			break;
		}
		memmove(a, cp->buf + o, m);
		n -= m;
		offset += m;
		a += m;
	}
	poperror();
	free(buf);
	cp->dp = 0;
	qunlock(cp->ctlrlock);
	return len-n;
}

static long
atapirwio(Chan *c, char *a, ulong len, ulong offset)
{
	Drive *dp;
	ulong vers;
	long rv;

	dp = atadrive[DRIVE(c->qid.path)];

	qlock(dp);
	if(waserror()){
		qunlock(dp);
		nexterror();
	}

	vers = c->qid.vers;
	c->qid.vers = dp->vers;
	if(vers && vers != dp->vers)
		error(Eio);
	rv = atapiio(dp, a, len, offset);

	poperror();
	qunlock(dp);
	return rv;
}

static void
atapipart(Drive *dp)
{
	Controller *cp;
	uchar *buf, err;
	Partition *pp;
	int retrycount;

	cp = dp->cp;

	pp = &dp->p[0];
	strcpy(pp->name, "disk");
	pp->start = 0;
	pp->end = 0;
	dp->npart = 1;

	buf = smalloc(Maxxfer);
	qlock(cp->ctlrlock);
	retrycount = 2;
retry:
	if(waserror()){
		DPRINT("atapipart: cmd %uX error %uX\n", cp->cmd, cp->error);
		if((cp->status & Serr) && (cp->error & 0xF0) == 0x60){
			dp->vers++;
			if(retrycount){
				retrycount--;
				goto retry;
			}
		}
		cp->dp = 0;
		free(buf);
		if((cp->status & Serr) && (cp->error & 0xF0) == 0x20)
			err = cp->error & 0xF0;
		else
			err = 0;
		qunlock(cp->ctlrlock);
		if(err == 0x20)
			return;
		nexterror();
	}

	cp->buf = buf;
	cp->dp = dp;

	cp->len = 18;
	cp->count = 0;
	memset(cp->cmdblk, 0, sizeof(cp->cmdblk));
	cp->cmdblk[0] = Creqsense;
	cp->cmdblk[4] = 18;
	atapiexec(dp);
	if(cp->count != 18){
		print("cmd=%2.2uX, lastcmd=%2.2uX ", cp->cmd, cp->lastcmd);
		print("cdsize count %d, status 0x%2.2uX, error 0x%2.2uX\n",
			cp->count, cp->status, cp->error);
		error(Eio);
	}

	cp->len = 8;
	cp->count = 0;
	memset(cp->cmdblk, 0, sizeof(cp->cmdblk));
	cp->cmdblk[0] = Ccapacity;
	atapiexec(dp);
	if(cp->count != 8){
		print("cmd=%2.2uX, lastcmd=%2.2uX ", cp->cmd, cp->lastcmd);
		print("cdsize count %d, status 0x%2.2uX, error 0x%2.2uX\n",
			cp->count, cp->status, cp->error);
		error(Eio);
	}
	dp->lbasecs = (cp->buf[0]<<24)|(cp->buf[1]<<16)|(cp->buf[2]<<8)|cp->buf[3];
	dp->cap = dp->lbasecs*dp->bytes;
	cp->dp = 0;
	free(cp->buf);
	poperror();
	qunlock(cp->ctlrlock);

	pp->end = dp->cap / dp->bytes;
	dp->partok = 1;
}

static void
atapiintr(Controller *cp)
{
	uchar cause;
	int count, loop, pbase;
	uchar *addr;

	pbase = cp->pbase;
	cause = inb(pbase+Pcount) & 0x03;
	DPRINT("%s: atapiintr %uX\n", cp->dp->vol, cause);
	switch(cause){

	case 0:						/* data out */
		cp->status |= Serr;
		/*FALLTHROUGH*/
	case 1:						/* command */
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(pbase+Perror);
			wakeup(&cp->r); 
			break;
		}
		outss(pbase+Pdata, cp->cmdblk, sizeof(cp->cmdblk)/2);
		break;

	case 2:						/* data in */
		addr = cp->buf;
		if(addr == 0){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			if(cp->status & Serr)
				cp->error = inb(pbase+Perror);
			wakeup(&cp->r);	 
			break;	
		}
		loop = 0;
		while((cp->status & (Serr|Sdrq)) == 0){
			if(++loop > Maxloop){
				cp->status |= Serr;
				break;
			}
			cp->status = inb(pbase+Pstatus);
		}
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(pbase+Perror);
			print("%s: Cpktcmd status=%uX, error=%uX\n",
				cp->dp->vol, cp->status, cp->error);
			wakeup(&cp->r);
			break;
		}
		count = inb(pbase+Pcyllsb)|(inb(pbase+Pcylmsb)<<8);
		if (count > Maxxfer) 
			count = Maxxfer;
		inss(pbase+Pdata, addr, count/2);
		cp->count = count;
		cp->lastcmd = cp->cmd; 
		break;

	case 3:						/* status */
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
		if(cp->status & Serr)
			cp->error = inb(cp->pbase+Perror);
		wakeup(&cp->r);	
		break;
	}
}
