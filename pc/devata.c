/*
 * This has gotten a bit messy with the addition of multiple controller
 * and ATAPI support; needs a rewrite before adding any 'ctl' functions.
 * The register locking needs looked at.
 * The PCI hacks are truly awful.
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#define DEBUG		0
#define DPRINT 		if(DEBUG)print
#define XPRINT 		if(DEBUG)print
#define ILOCK(x)	ilock(x)
#define IUNLOCK(x)	iunlock(x)

typedef	struct Drive		Drive;
typedef	struct Ident		Ident;
typedef	struct Controller	Controller;
typedef struct Partition	Partition;
typedef struct Repl		Repl;
typedef struct Atapicmd		Atapicmd;

enum
{
	/* ports */
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

	Pctl=		2,	/* device control, alternate status */
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
	Cwrite2=	0x2A,

	/* disk states */
	Sspinning,
	Sstandby,

	/* file types */
	Qdir=		0,

	Maxxfer=	BY2PG,		/* maximum transfer size/cmd */
	Npart=		20+2,		/* 8 sub partitions, disk, and partition */
	Nrepl=		64,		/* maximum replacement blocks */

	Hardtimeout=	6000,		/* disk access timeout (ms) */
	Atapitimeout=	10000,		/* disk access timeout (ms) */
	NCtlr=		8,		/* not really */
	NDrive=		NCtlr*2,

	/* cd files */
	CDdisk = 0,
	CDcmd,
	CDdata,
	CDmax,
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
 * the result of the last user-invoked atapi cmd
 */
struct Atapicmd
{
	QLock;
	int	pid;
	ushort	status;
	ushort	error;
	uchar	cmdblk[12];
};

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

	vlong	cap;		/* total bytes */
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
	Atapicmd atapicmd;
};

/*
 *  a controller for 2 drives
 */
struct Controller
{
	QLock*	ctlrlock;	/* exclusive access to the controller */

	Lock	reglock;	/* exclusive access to the registers */

	int	cmdport;	/* base port */
	int	ctlport;
	uchar	ctlrno;
	int	tbdf;

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
static int have640b;

typedef struct Atadev Atadev;
typedef struct Atadev {
	int	cmdport;
	int	ctlport;
	int	irq;

	Pcidev*	p;
	void	(*ienable)(Atadev*);
} Atadev;

static void pc87415ienable(Atadev*);

static Atadev atadev[NCtlr] = {
	{ 0x1F0, 0x3F4, 14, },	/* primary */
	{ 0x170, 0x374, 15, },	/* secondary */
	{ 0x1E8, 0x3EC,  0, },	/* tertiary */
	{ 0x168, 0x36C,  0, },	/* quaternary */
};
static int natadev = 4;

static void	ataintr(Ureg*, void*);
static long	ataxfer(Drive*, Partition*, int, vlong, long, uchar*);
static void	ataident(Drive*);
static void	atafeature(Drive*, uchar);
static void	ataparams(Drive*);
static void	atapart(Drive*);
static int	ataprobe(Drive*, int, int, int);
static void	atasleep(Controller*, int);
static void	ataclock(void);

static int	isatapi(Drive*);
static long	atapirwio(Chan*, uchar*, ulong, vlong, int);
static void	atapipart(Drive*);
static void	atapiintr(Controller*);
static void	atapiexec(Drive*);

static int
atagen(Chan* c, Dirtab*, int, int s, Dir* dirp)
{
	Qid qid;
	int drive;
	char name[NAMELEN+4];
	Drive *dp;
	Partition *pp;
	vlong l;

	if(s == DEVDOTDOT){
		devdir(c, (Qid){CHDIR, 0}, "#H", 0, eve, 0555, dirp);
		return 1;
	}

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
	l = (pp->end - pp->start) * (vlong)dp->bytes;
	devdir(c, qid, name, l, eve, 0660, dirp);
	return 1;
}

static void
pc87415ienable(Atadev* devp)
{
	Pcidev *p;
	int x;

	p = devp->p;
	if(p == nil)
		return;

	x = pcicfgr32(p, 0x40);
	if(devp->cmdport == (p->mem[0].bar & ~0x01))
		x &= ~0x00000100;
	else
		x &= ~0x00000200;
	pcicfgw32(p, 0x40, x);
}

static void
atareset(void)
{
	Pcidev *p;
	int ccrp, r;

	p = nil;
	while(p = pcimatch(p, 0, 0)){
		if(p->vid == 0x1095 && p->did == 0x0640){
			/*
			 * CMD640B dual PCI controllers. Amongst other
			 * bugs only one of the controllers can be active at a time.
			 * Unfortunately there's no way to tell which pair of
			 * controllers this is, so if one is found then all controller
			 * pairs are synchronised.
			 */
			have640b++;

			/*
			 * Make sure read-ahead is disabled on all
			 * drives and that the 2nd controller is enabled:
			 *   reg 0x51:	bit 7 - drive 1 read ahead disable
			 *  		bit 6 - drive 0 read ahead disable
			 *  		bit 3 - 2nd controller enable
			 *   reg 0x57:	bit 3 - drive 1 read ahead disable
			 *  		bit 2 - drive 0 read ahead disable
			 */
			r = pcicfgr8(p, 0x51);
			r |= 0xC8;
			pcicfgw8(p, 0x51, r);
			r = pcicfgr8(p, 0x57);
			r |= 0x0C;
			pcicfgw8(p, 0x57, r);
		}
		else if(p->vid == 0x1042 && (p->did == 0x1000 || p->did == 0x1001)){
			/*
			 * PC-Tech RZ1000 controllers.
			 * Turn off prefetch.
			 * This is overkill, but cheap.
			 */
			r = pcicfgr32(p, 0x40);
			r &= ~0x2000;
			pcicfgw32(p, 0x40, r);
		}
		else if(p->vid == 0x100B && p->did == 0x0002){
			/*
			 * National Semiconductor PC87415.
			 * Disable interrupts on both channels until
			 * after they are probed for drives.
			 * This must be called before interrupts are
			 * enabled in case the IRQ is being shared.
			 */
			pcicfgw32(p, 0x40, 0x00000300);

			/*
			 * Add any native-mode channels to the list to
			 * be probed.
			 */
			ccrp = pcicfgr8(p, PciCCRp);
			if((ccrp & 0x01) && natadev < nelem(atadev)){
				atadev[natadev].cmdport = p->mem[0].bar & ~0x01;
				atadev[natadev].ctlport = p->mem[1].bar & ~0x01;
				atadev[natadev].irq = p->intl;
				atadev[natadev].p = p;
				atadev[natadev].ienable = pc87415ienable;
				natadev++;
			}
			if((ccrp & 0x04) && natadev < nelem(atadev)){
				atadev[natadev].cmdport = p->mem[2].bar & ~0x01;
				atadev[natadev].ctlport = p->mem[3].bar & ~0x01;
				atadev[natadev].irq = p->intl;
				atadev[natadev].p = p;
				atadev[natadev].ienable = pc87415ienable;
				natadev++;
			}
		}
	}
}

static int
atactlrwait(Controller* ctlr, uchar pdh, uchar ready, ulong ticks)
{
	int port;
	uchar dh, status;

	port = ctlr->cmdport;
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

	DPRINT("ata%d: ctlrwait failed 0x%uX\n", ctlr->ctlrno, status);
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
atactlrprobe(int ctlrno, Atadev* devp, int irq, int resetok)
{
	Controller *ctlr;
	int atapi, cmdport, ctlport, mask, once, timo;
	uchar error, status, msb, lsb;
	char name[13];

	cmdport = devp->cmdport;
	ctlport = devp->ctlport;

	/*
	 * Check the existence of a controller by verifying a sensible
	 * value can be written to and read from the drive/head register.
	 * If it's OK, allocate and initialise a Controller structure.
	 */
	DPRINT("ata%d: port 0x%uX\n", ctlrno, cmdport);
	outb(cmdport+Pdh, DHmagic);
	for(timo = 30000; timo; timo--){
		microdelay(1);
		status = inb(cmdport+Pdh);
		if(status == DHmagic)
			break;
	}
	status = inb(cmdport+Pdh);
	if(status != DHmagic){
		DPRINT("ata%d: DHmagic not ok == 0x%uX, 0x%uX\n",
			ctlrno, status, inb(cmdport+Pstatus));
		return -1;
	}
	DPRINT("ata%d: DHmagic ok\n", ctlrno);
	if((ctlr = xalloc(sizeof(Controller))) == 0)
		return -1;
	ctlr->cmdport = cmdport;
	ctlr->ctlport = ctlport;
	ctlr->ctlrno = ctlrno;
	ctlr->tbdf = BUSUNKNOWN;
	ctlr->lastcmd = 0xFF;

	/*
	 * Attempt to check the existence of drives on the controller
	 * by issuing a 'check device diagnostics' command.
	 * Issuing a device reset here would possibly destroy any BIOS
	 * drive remapping and, anyway, some controllers (Vibra16) don't
	 * seem to implement the control-block registers; do it if requested.
	 * At least one controller/ATAPI-drive combination doesn't respond
	 * to the Cedd (Micronics M54Li + Sanyo CRD-254P) so let's check for the
	 * ATAPI signature straight off. If we find it there will be no probe
	 * done for a slave. Tough.
	 */
	if(resetok && ctlport){
		outb(ctlport+Pctl, Srst|nIEN);
		delay(10);
		outb(ctlport+Pctl, 0);
		if(atactlrwait(ctlr, DHmagic, 0, MS2TK(20))){
			DPRINT("ata%d: Srst status 0x%uX/0x%uX/0x%uX\n", ctlrno,
				inb(cmdport+Pstatus), inb(cmdport+Pcylmsb), inb(cmdport+Pcyllsb));
			xfree(ctlr);
			return -1;
		}
	}

	/*
	 * Disable interrupts.
	 */
	outb(ctlport+Pctl, nIEN);

	once = 1;
retry:
	atapi = 0;
	mask = 0;
	status = inb(cmdport+Pstatus);
	DPRINT("ata%d: ATAPI 0x%uX 0x%uX 0x%uX\n", ctlrno, status,
		inb(cmdport+Pcylmsb), inb(cmdport+Pcyllsb));
	USED(status);
	if(/*status == 0 &&*/ inb(cmdport+Pcylmsb) == 0xEB && inb(cmdport+Pcyllsb) == 0x14){
		DPRINT("ata%d: ATAPI ok\n", ctlrno);
		atapi |= 0x01;
		mask |= 0x01;
		goto atapislave;
	}
	if(atactlrwait(ctlr, DHmagic, 0, MS2TK(1))){
		DPRINT("ata%d: Cedd status 0x%uX/0x%uX/0x%uX\n", ctlrno,
			inb(cmdport+Pstatus), inb(cmdport+Pcylmsb), inb(cmdport+Pcyllsb));
		if(once){
			once = 0;
			ctlr->cmd = 0;
			goto retry;
		}
		xfree(ctlr);
		return -1;
	}

	/*
	 * Can only get here if controller is not busy.
	 * If there are drives Sbusy will be set within 400nS.
	 * Wait for the command to complete (6 seconds max).
	 */
	ctlr->cmd = Cedd;
	outb(cmdport+Pcmd, Cedd);
	microdelay(1);
	status = inb(cmdport+Pstatus);
	if(!(status & Sbusy)){
		DPRINT("ata%d: !busy 1 0x%uX\n", ctlrno, status);
		xfree(ctlr);
		return -1;
	}
	for(timo = 6000; timo; timo--){
		status = inb(cmdport+Pstatus);
		if(!(status & Sbusy))
			break;
		delay(1);
	}
	DPRINT("ata%d: timo %d\n", ctlrno, 6000-timo);
	status = inb(cmdport+Pstatus);
	if(status & Sbusy){
		DPRINT("ata%d: busy 2 0x%uX\n", ctlrno, status);
		xfree(ctlr);
		return -1;
	}

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
	 * When checking status, mask off the IDX bit.
	 */
	error = inb(cmdport+Perror);
	DPRINT("ata%d: master diag status 0x%uX, error 0x%uX\n",
		ctlr->ctlrno, inb(cmdport+Pstatus), error);
	if((error & ~0x80) == 0x01)
		mask |= 0x01;

atapislave:
	outb(cmdport+Pdh, DHmagic|DHslave);
	microdelay(1);
	status = inb(cmdport+Pstatus);
	error = inb(cmdport+Perror);
	DPRINT("ata%d: slave diag status 0x%uX, error 0x%uX\n",
		ctlr->ctlrno, status, error);
	if((status & ~0x02) && (status & (Sbusy|Serr)) == 0 && (error & ~0x80) == 0x01)
		mask |= 0x02;
	else if(status == 0){
		msb = inb(cmdport+Pcylmsb);
		lsb = inb(cmdport+Pcyllsb);
		DPRINT("ata%d: ATAPI slave 0x%uX 0x%uX 0x%uX\n", ctlrno, status,
			inb(cmdport+Pcylmsb), inb(cmdport+Pcyllsb));
		if(msb == 0xEB && lsb == 0x14){
			atapi |= 0x02;
			mask |= 0x02;
		}
	}
	outb(cmdport+Pdh, DHmagic);

//skipslave:
	if(mask == 0){
		xfree(ctlr);
		return -1;
	}
	sprint(name, "ata%dcmd", ctlrno);
	if(ioalloc(devp->cmdport, 0x8, 0, name) < 0){
		print("#H%d: cmd port %d in use", ctlrno, devp->cmdport);
		xfree(ctlr);
		return -1;
	}
	sprint(name, "ata%dctl", ctlrno);
	if(ioalloc(devp->ctlport+Pctl, 1, 0, name) < 0){
		iofree(devp->cmdport);
		print("#H%d: ctl port %d in use", ctlrno, devp->ctlport);
		xfree(ctlr);
		return -1;
	}
	atactlr[ctlrno] = ctlr;

	if(have640b && (ctlrno & 0x01))
		ctlr->ctlrlock = &atactlrlock[ctlrno-1];
	else
		ctlr->ctlrlock = &atactlrlock[ctlrno];

	if(mask & 0x01)
		atadrivealloc(ctlr, ctlrno*2, atapi & 0x01);
	if(mask & 0x02)
		atadrivealloc(ctlr, ctlrno*2+1, atapi & 0x02);

	print("#H%d: cmdport 0x%uX ctlport 0x%uX irq %d mask 0x%uX atapi 0x%uX\n",
		ctlrno, cmdport, ctlport,  irq, mask, atapi);
	snprint(name, sizeof name, "ata%d", ctlrno);
	intrenable(irq, ataintr, ctlr, ctlr->tbdf, name);
	inb(cmdport+Pstatus);
	outb(ctlport+Pctl, 0);
	if(devp->ienable)
		devp->ienable(devp);

	return 0;
}

static void
atactlrreset(void)
{
	int ctlrno, devno, driveno, i, resetok, slave, spindown;
	ISAConf isa;
	Atadev *devp;

	ctlrno = 0;
	for(devno = 0; devno < natadev; devno++){
		devp = &atadev[devno];
		memset(&isa, 0, sizeof(ISAConf));
		isaconfig("ata", ctlrno, &isa);
		if(isa.port && isa.port != devp->cmdport)
			continue;
		if(isa.irq == 0 && (isa.irq = devp->irq) == 0)
			continue;

		driveno = resetok = spindown = 0;
		for(i = 0; i < isa.nopt; i++){
			DPRINT("ata%d: opt %s\n", ctlrno, isa.opt[i]);
			if(cistrncmp(isa.opt[i], "spindown", 8) == 0){
				if(isa.opt[i][9] != '=')
					continue;
				if(isa.opt[i][8] == '0')
					slave = 0;
				else if(isa.opt[i][8] == '1')
					slave = 1;
				else
					continue;
				if((spindown = strtol(&isa.opt[i][10], 0, 0)) == 0)
					continue;
				if(spindown < (Hardtimeout+2000)/1000)
					spindown = (Hardtimeout+2000)/1000;
				driveno = ctlrno*2+slave;
			}
			else if(cistrcmp(isa.opt[i], "reset") == 0)
				resetok = 1;
		}

		if(atactlrprobe(ctlrno, devp, isa.irq, resetok))
			continue;
		ctlrno++;

		if(spindown == 0 || atadrive[driveno] == 0)
			continue;
		atadrive[driveno]->spindown = spindown;
		spindownmask |= (1<<driveno);
		DPRINT("ata%d: opt spindownmask 0x%uX\n", ctlrno, spindownmask);
	}

	if(spindownmask)
		addclock0link(ataclock);
}

/*
 *  Get the characteristics of each drive.  Mark unresponsive ones
 *  off line.
 */
static Chan*
ataattach(char* spec)
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

static int
atawalk(Chan* c, char* name)
{
	return devwalk(c, name, 0, 0, atagen);
}

static void
atastat(Chan* c, char* dp)
{
	devstat(c, dp, 0, 0, atagen);
}

static Chan*
ataopen(Chan* c, int omode)
{
	return devopen(c, omode, 0, 0, atagen);
}

void
ataclose(Chan* c)
{
	Drive *dp;
	Partition *p;
	Atapicmd *acmd;

	if(c->mode != OWRITE && c->mode != ORDWR)
		return;

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp == 0)
		return;
	p = &dp->p[PART(c->qid.path)];
	if(dp->atapi == 1 && strcmp(p->name, "cmd") == 0) {
		acmd = &dp->atapicmd;
		if(canqlock(acmd)) {
			qunlock(acmd);
			return;
		}
		if(acmd->pid == up->pid)
			qunlock(acmd);
		return;
	}
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

static long
ataread(Chan* c, void* a, long n, vlong off)
{
	Drive *dp;
	long rv, i;
	int skip;
	uchar *aa = a;
	Partition *pp;
	uchar *buf;
	Atapicmd *acmd;
	Controller *cp;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, 0, 0, atagen);

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp->atapi){
		switch(PART(c->qid.path)){
		case CDdisk:
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
			break;
		case CDcmd:
			acmd = &dp->atapicmd;
			if(n < 4)
				error(Ebadarg);
			if(canqlock(acmd)) {
				qunlock(acmd);
				error(Egreg);
			}
			if(acmd->pid != up->pid)
				error(Egreg);
			n = 4;
			*aa++ = 0;
			*aa++ = 0;
			*aa++ = acmd->error;
			*aa   = acmd->status;
			qunlock(acmd);
			return n;
		case CDdata:
			acmd = &dp->atapicmd;
			if(canqlock(acmd)) {
				qunlock(acmd);
				error(Egreg);
			}
			if(acmd->pid != up->pid)
				error(Egreg);
			if(n > Maxxfer)
				error(Ebadarg);

			cp = dp->cp;
			qlock(cp->ctlrlock);
			cp->len = 0;
			cp->buf = 0;
			cp->dp = dp;
			if(waserror()) {
				if(cp->buf)
					free(cp->buf);
				cp->buf = 0;
				cp->dp = 0;
				acmd->status = cp->status;
				acmd->error = cp->error;
				qunlock(cp->ctlrlock);
				nexterror();
			}
			if(n)
				cp->buf = smalloc(Maxxfer);
			cp->len = n;
			memmove(cp->cmdblk, acmd->cmdblk, sizeof cp->cmdblk);
			atapiexec(dp);
			memmove(a, cp->buf, cp->count);
			poperror();
			if(cp->buf)
				free(cp->buf);
			acmd->status = cp->status;
			acmd->error = cp->error;
			n = cp->count;
			qunlock(cp->ctlrlock);
			return n;
		}
	}
	pp = &dp->p[PART(c->qid.path)];

	if(off < 0)
		error(Ebadarg);

	buf = smalloc(Maxxfer);
	if(waserror()){
		free(buf);
		nexterror();
	}

	skip = off % dp->bytes;
	for(rv = 0; rv < n; rv += i){
		if(dp->atapi)
			i = atapirwio(c, buf, n-rv+skip, off+rv-skip, Cread2);
		else
			i = ataxfer(dp, pp, Cread, off+rv-skip, n-rv+skip, buf);
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

static long
atawrite(Chan *c, void *a, long n, vlong off)
{
	Drive *dp;
	long rv, i, partial;
	uchar *aa = a;
	Partition *pp;
	uchar *buf;
	Atapicmd *acmd;

	if(c->qid.path == CHDIR)
		error(Eisdir);

	dp = atadrive[DRIVE(c->qid.path)];
	if(dp->atapi){
		switch(PART(c->qid.path)) {
		case CDdisk:
			if(dp->online == 0 || dp->atapi == 1)
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
			break;
		case CDcmd:
			if(n != 12)
				error(Ebadarg);
			acmd = &dp->atapicmd;
			qlock(acmd);
			acmd->pid = up->pid;
			memmove(acmd->cmdblk, a, n);
			return n;
		case CDdata:
			error(Egreg);
		}
	}
	pp = &dp->p[PART(c->qid.path)];

	if(off < 0)
		error(Ebadarg);

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
	partial = off % dp->bytes;
	if(partial){
		if(dp->atapi)
			atapirwio(c, buf, dp->bytes, off-partial, Cread2);
		else
			ataxfer(dp, pp, Cread, off-partial, dp->bytes, buf);
		if(partial+n > dp->bytes)
			rv = dp->bytes - partial;
		else
			rv = n;
		memmove(buf+partial, aa, rv);
		if(dp->atapi)
			atapirwio(c, buf, dp->bytes, off-partial, Cwrite2);
		else
			ataxfer(dp, pp, Cwrite, off-partial, dp->bytes, buf);
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
		if(dp->atapi)
			i = atapirwio(c, buf, i, off+rv, Cwrite2);
		else
			i = ataxfer(dp, pp, Cwrite, off+rv, i, buf);
		if(i == 0)
			break;
	}

	/*
	 *  if not ending on a sector boundary,
	 *  read in the last sector before writing
	 *  it out.
	 */
	if(partial){
		if(dp->atapi)
			atapirwio(c, buf, dp->bytes, off+rv, Cread2);
		else
			ataxfer(dp, pp, Cread, off+rv, dp->bytes, buf);
		memmove(buf, aa+rv, partial);
		if(dp->atapi)
			atapirwio(c, buf, dp->bytes, off+rv, Cwrite2);
		else
			ataxfer(dp, pp, Cwrite, off+rv, dp->bytes, buf);
		rv += partial;
	}

	free(buf);
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
			DPRINT("%s: found bblk %ld at offset %d\n",
			dp->vol, bblk, i);
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
ataxfer(Drive *dp, Partition *pp, int cmd, vlong off, long len, uchar *buf)
{
	Controller *cp;
	long lblk;
	int cyl, sec, head;
	int loop, stat;
	ulong start;

	if(dp->online == 0)
		error(Eio);

	/*
	 *  cut transfer size down to disk buffer size
	 */
	start = off / dp->bytes;
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

	XPRINT("%s: ataxfer cyl %d sec %d head %d len %ld\n",
		dp->vol, cyl, sec, head, len);

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

	outb(cp->cmdport+Pcount, cp->nsecs);
	outb(cp->cmdport+Psector, sec);
	outb(cp->cmdport+Pdh, dp->dh | (dp->lba<<6) | head);
	outb(cp->cmdport+Pcyllsb, cyl);
	outb(cp->cmdport+Pcylmsb, cyl>>8);
	outb(cp->cmdport+Pcmd, cmd);

	if(cmd == Cwrite){
		loop = 0;
		microdelay(1);
		while((stat = inb(cp->cmdport+Pstatus) & (Serr|Sdrq)) == 0){
			microdelay(1);
			if(++loop > 10000)
				panic("%s: ataxfer", dp->vol);
		}
		outss(cp->cmdport+Pdata, cp->buf, dp->bytes/2);
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
		DPRINT("%s err: lblk %ld status 0x%uX, err 0x%uX\n",
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
	outb(cp->cmdport+Pfeature, arg);
	outb(cp->cmdport+Pdh, dp->dh);
	outb(cp->cmdport+Pcmd, Cfeature);
	IUNLOCK(&cp->reglock);

	atasleep(cp, Hardtimeout);

	if(cp->status & Serr)
		DPRINT("%s: setbuf err: status 0x%uX, err 0x%uX\n",
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
	outb(cp->cmdport+Pdh, dp->dh);
	outb(cp->cmdport+Pcmd, cmd);
	IUNLOCK(&cp->reglock);

	DPRINT("%s: ident command 0x%uX sent\n", dp->vol, cmd);
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
	//if (cp->cmd == Cident2)
	//	tsleep(&cp->r, return0, 0, Hardtimeout);

	memmove(id, ip->model, sizeof(id)-1);
	id[sizeof(id)-1] = 0;

	DPRINT("%s: config 0x%uX capabilities 0x%uX\n",
		dp->vol, ip->config, ip->capabilities);
	if(dp->atapi){
		dp->bytes = 2048;
		if((ip->config & 0x0060) == 0x0020)
			dp->drqintr = 1;
		if((ip->config & 0x1F00) == 0x0000)
			dp->atapi = 2;
	}
	if(dp->spindown && (ip->capabilities & (1<<13)))
		dp->spindown /= 5;

	/* use default (unformatted) settings */
	dp->cyl = ip->cyls;
	dp->heads = ip->heads;
	dp->sectors = ip->s2t;

	if(ip->cvalid&(1<<0)){
		/* use current settings */
		dp->cyl = ip->ccyls;
		dp->heads = ip->cheads;
		dp->sectors = ip->cs2t;
		XPRINT("%s: %d/%d/%d changed to %ld/%d/%d CHS\n",
			dp->vol,
			ip->cyls, ip->heads, ip->s2t,
			dp->cyl, dp->heads, dp->sectors);
	}

	lbasecs = (ip->lbasecs[0]) | (ip->lbasecs[1]<<16);
	if((ip->capabilities & (1<<9)) && (lbasecs & 0xf0000000) == 0){
		dp->lba = 1;
		dp->lbasecs = lbasecs;
		dp->cap = (vlong)dp->bytes * dp->lbasecs;
		XPRINT("%s: LBA: %s %lud sectors %lld bytes\n",
			dp->vol, id, dp->lbasecs,
			dp->cap);
	} else {
		dp->lba = 0;
		dp->lbasecs = 0;
		dp->cap = (vlong)dp->bytes * dp->cyl * dp->heads * dp->sectors;
	}
	XPRINT("%s: %s %ld/%d/%d CHS %lld bytes\n",
		dp->vol, id, dp->cyl, dp->heads, dp->sectors,
		dp->cap);

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

	outb(cp->cmdport+Pcount, 1);
	outb(cp->cmdport+Psector, sec+1);
	outb(cp->cmdport+Pdh, dp->dh | (dp->lba<<6) | head);
	outb(cp->cmdport+Pcyllsb, cyl);
	outb(cp->cmdport+Pcylmsb, cyl>>8);
	outb(cp->cmdport+Pcmd, Cread);
	IUNLOCK(&cp->reglock);

	atasleep(cp, Hardtimeout);

	if(cp->status & Serr){
		DPRINT("%s: probe err: status 0x%uX, err 0x%uX\n",
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
	dp->cap = (vlong)dp->bytes * dp->cyl * dp->heads * dp->sectors;
	DPRINT("%s: probed: %ld/%d/%d CHS %lld bytes\n",
		dp->vol, dp->cyl, dp->heads, dp->sectors,
		dp->cap);
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
	n = getfields((char*)buf, line, Nrepl+1, 1, "\n");
	if(strncmp(line[0], REPLMAGIC, sizeof(REPLMAGIC)-1)){
		dp->repl.p = 0;
	} else {
		for(dp->repl.nrepl = 0, i = 1; i < n; i++, dp->repl.nrepl++){
			if(getfields(line[i], field, 1, 1, " ") != 1)
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
		ataxfer(dp, &dp->p[0], Cread, (dp->p[0].end-2)*
			(vlong)dp->bytes, dp->bytes, buf);
		buf[dp->bytes-1] = 0;
		n = getfields((char*)buf, line, Npart+1, 1, "\n");
		if(n > 0 && strncmp(line[0], PARTMAGIC, sizeof(PARTMAGIC)-1) == 0)
			i = 1;
		else{
			ataxfer(dp, &dp->p[1], Cread, 0, dp->bytes, buf);
			buf[dp->bytes-1] = 0;
			n = getfields((char*)buf, line, Npart+1, 1, "\n");
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
		n = getfields((char*)buf, line, Npart+1, 1, "\n");
	}

	/*
	 *  parse partition table.
	 */
	if(n > 0 && strncmp(line[0], PARTMAGIC, sizeof(PARTMAGIC)-1) == 0){
		for(i = 1; i < n; i++){
			if(getfields(line[i], field, 3, 1, " ") != 3)
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
	while((cp->status = inb(cp->cmdport+Pstatus)) & Sbusy){
		if(++loop > Maxloop){
			print("ata%d: cmd=0x%uX, lastcmd=0x%uX status=0x%uX\n",
				cp->ctlrno, cp->cmd, cp->lastcmd, inb(cp->cmdport+Pstatus));
			panic("%s: wait busy\n", dp->vol);
		}
		microdelay(1);
	}

	switch(cp->cmd){
	case Cwrite:
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cp->cmdport+Perror);
			wakeup(&cp->r);
			break;
		}
		cp->sofar++;
		if(cp->sofar < cp->nsecs){
			loop = 0;
			while(((cp->status = inb(cp->cmdport+Pstatus)) & Sdrq) == 0){
				if(++loop > Maxloop)
					panic("%s: write cmd=%lux status=%lux\n",
						dp->vol, cp->cmd, inb(cp->cmdport+Pstatus));
				microdelay(1);
			}
			addr = cp->buf;
			if(addr){
				addr += cp->sofar*dp->bytes;
				outss(cp->cmdport+Pdata, addr, dp->bytes/2);
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
				print("%s: read/ident cmd=0x%uX status=0x%uX\n",
					dp->vol, cp->cmd, inb(cp->cmdport+Pstatus));
				cp->status |= Serr;
				break;
			}
			microdelay(1);
			cp->status = inb(cp->cmdport+Pstatus);
		}
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cp->cmdport+Perror);
			wakeup(&cp->r);
			break;
		}
		addr = cp->buf;
		if(addr){
			addr += cp->sofar*dp->bytes;
			inss(cp->cmdport+Pdata, addr, dp->bytes/2);
		}
		cp->sofar++;
		if(cp->sofar > cp->nsecs)
			print("%s: intr %d %d\n", dp->vol, cp->sofar, cp->nsecs);
		if(cp->sofar >= cp->nsecs){
			cp->lastcmd = cp->cmd;
			//if(cp->cmd != Cread)
			//	cp->cmd = Cident2;
			//else
				cp->cmd = 0;
			inb(cp->cmdport+Pstatus);
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
		DPRINT("Cident2\n");
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
		break;
	case Cpktcmd:
		atapiintr(cp);
		break;
	default:
		if(cp->cmd == 0 && cp->lastcmd == Cpktcmd)
			break;
		if(cp->status & Serr)
			cp->error = inb(cp->cmdport+Perror);
		print("%s: weird interrupt, cmd=%.2ux, lastcmd=%.2ux, ",
			dp->vol, cp->cmd, cp->lastcmd);
		print("status=%.2ux, error=%.2ux, count=%.2ux\n",
			cp->ctlrno, cp->error, inb(cp->cmdport+Pcount));
		break;
	}

	IUNLOCK(&cp->reglock);
}

static void
ataclock(void)
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
			outb(cp->cmdport+Pcount, 0);
			outb(cp->cmdport+Pdh, dp->dh);
			outb(cp->cmdport+Pcmd, cp->cmd);
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
	outb(cp->cmdport+Pdh, dp->dh);
	DPRINT("%s: isatapi %d\n", dp->vol, dp->atapi);
	outb(cp->cmdport+Pcmd, 0x08);
	if(atactlrwait(dp->cp, DHmagic, 0, MS2TK(100))){
		DPRINT("%s: isatapi ctlrwait status 0x%uX\n", dp->vol, inb(cp->cmdport+Pstatus));
		return 0;
	}
	dp->atapi = 0;
	dp->bytes = 512;
	microdelay(1);
	if(inb(cp->cmdport+Pstatus)){
		DPRINT("%s: isatapi status 0x%uX\n", dp->vol, inb(cp->cmdport+Pstatus));
		return 0;
	}
	if(inb(cp->cmdport+Pcylmsb) != 0xEB || inb(cp->cmdport+Pcyllsb) != 0x14){
		DPRINT("%s: isatapi cyl 0x%uX 0x%uX\n",
			dp->vol, inb(cp->cmdport+Pcylmsb), inb(cp->cmdport+Pcyllsb));
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
		print("cmdreadywait fails");
		error(Eio);
	}
	
	ILOCK(&cp->reglock);
	cp->count = 0;
	cp->sofar = 0;
	cp->error = 0;
	cp->cmd = Cpktcmd;
	outb(cp->cmdport+Pcount, 0);
	outb(cp->cmdport+Psector, 0);
	outb(cp->cmdport+Pfeature, 0);
	outb(cp->cmdport+Pcyllsb, cp->len);
	outb(cp->cmdport+Pcylmsb, cp->len>>8);
	outb(cp->cmdport+Pdh, dp->dh);
	outb(cp->cmdport+Pcmd, cp->cmd);

	if(dp->drqintr == 0){
		microdelay(1);
		for(loop = 0; (inb(cp->cmdport+Pstatus) & (Serr|Sdrq)) == 0; loop++){
			microdelay(1);
			if(loop < 10000)
				continue;
			panic("%s: cmddrqwait: cmd=%lux status=%lux\n",
				dp->vol, cp->cmd, inb(cp->cmdport+Pstatus));
		}
		outss(cp->cmdport+Pdata, cp->cmdblk, sizeof(cp->cmdblk)/2);
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
	atasleep(cp, Atapitimeout);
	poperror();
	if(loop)
		nexterror();

	if(cp->status & Serr){
		DPRINT("%s: Bad packet command 0x%uX, error 0x%uX\n",
			dp->vol, cp->cmdblk[0], cp->error);
		error(Eio);
	}
}

static void
atapireqsense(Drive* dp)
{
	Controller *cp;
	uchar *buf;

	cp = dp->cp;

	buf = smalloc(Maxxfer);
	cp->buf = buf;
	cp->dp = dp;

	if(waserror()){
		free(buf);
		return;
	}

	cp->nsecs = 1;
	cp->len = 18;
	memset(cp->cmdblk, 0, sizeof(cp->cmdblk));
	cp->cmdblk[0] = Creqsense;
	cp->cmdblk[4] = 18;
	atapiexec(dp);
	if(cp->count != 18){
		print("cmd=%2.2uX, lastcmd=%2.2uX ", cp->cmd, cp->lastcmd);
		print("cdsize count %d, status 0x%2.2uX, error 0x%2.2uX\n",
			cp->count, cp->status, cp->error);
	}

	poperror();
	free(buf);
}

static long
atapiio(Drive *dp, uchar *buf, ulong len, vlong off, int cmd)
{
	ulong start;
	Controller *cp;
	int retrycount;

	/*
	 *  cut transfer size down to disk buffer size
	 */
	start = off / dp->bytes;
	if(len > Maxxfer)
		len = Maxxfer;
	len = (len + dp->bytes - 1) / dp->bytes;
	if(len == 0)
		return 0;

	cp = dp->cp;
	qlock(cp->ctlrlock);
	retrycount = 2;

retry:
	if(waserror()){
		DPRINT("atapiio: cmd 0x%uX error 0x%uX\n", cp->cmdblk[0], cp->error);
		dp->partok = 0;
		if((cp->status & Serr) && (cp->error & 0xF0) == 0x60){
			atapireqsense(dp);
			dp->vers++;
			if(retrycount){
				retrycount--;
				goto retry;
			}
		}
		cp->dp = 0;
		cp->buf = 0;
		qunlock(cp->ctlrlock);
		nexterror();
	}

	cp->buf = buf;
	cp->nsecs = len;
	cp->len = len*dp->bytes;
	cp->cmd = cmd;
	cp->dp = dp;

	memset(cp->cmdblk, 0, 12);
	cp->cmdblk[0] = cmd;
	cp->cmdblk[2] = start>>24;
	cp->cmdblk[3] = start>>16;
	cp->cmdblk[4] = start>>8;
	cp->cmdblk[5] = start;
	cp->cmdblk[7] = cp->nsecs>>8;
	cp->cmdblk[8] = cp->nsecs;
	atapiexec(dp);
	if(cp->count != cp->len)
		print("short read\n");
	cp->dp = 0;
	cp->buf = 0;
	len = cp->count;
	qunlock(cp->ctlrlock);
	poperror();

	return len;
}

static long
atapirwio(Chan *c, uchar *a, ulong len, vlong off, int cmd)
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
	rv = atapiio(dp, a, len, off, cmd);

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

	if(dp->atapi == 1) { /* cd-rom */ 
		dp->npart = CDmax;
		pp = &dp->p[CDcmd];
		strcpy(pp->name, "cmd");
		pp->start = pp->end = 0;
		pp = &dp->p[CDdata];
		strcpy(pp->name, "data");
		pp->start = pp->end = 0;
	}

	buf = smalloc(Maxxfer);
	qlock(cp->ctlrlock);
	retrycount = 2;
retry:
	if(waserror()){
		DPRINT("atapipart: cmd 0x%uX error 0x%uX\n", cp->cmdblk[0], cp->error);
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

	cp->nsecs = 1;
	cp->len = 18;
	memset(cp->cmdblk, 0, sizeof(cp->cmdblk));
	cp->cmdblk[0] = Creqsense;
	cp->cmdblk[4] = 18;
	atapiexec(dp);
	if(cp->count != 18){
		print("cmd=0x%2.2uX, lastcmd=0x%2.2uX ", cp->cmd, cp->lastcmd);
		print("cdsize count %d, status 0x%2.2uX, error 0x%2.2uX\n",
			cp->count, cp->status, cp->error);
		error(Eio);
	}

	cp->nsecs = 1;
	cp->len = 8;
	memset(cp->cmdblk, 0, sizeof(cp->cmdblk));
	cp->cmdblk[0] = Ccapacity;
	atapiexec(dp);
	if(cp->count != 8){
		print("cmd=0x%2.2uX, lastcmd=0x%2.2uX ", cp->cmd, cp->lastcmd);
		print("cdsize count %d, status 0x%2.2uX, error 0x%2.2uX\n",
			cp->count, cp->status, cp->error);
		error(Eio);
	}
	dp->lbasecs = (cp->buf[0]<<24)|(cp->buf[1]<<16)|(cp->buf[2]<<8)|cp->buf[3];
	dp->bytes = (cp->buf[4]<<24)|(cp->buf[5]<<16)|(cp->buf[6]<<8)|cp->buf[7];
	if(dp->bytes > 2048 && dp->bytes <= 2352)
		dp->bytes = 2048;
	dp->cap = dp->lbasecs*dp->bytes;
	DPRINT("%s: atapipart secs %lud, bytes %ud, cap %lld\n",
		dp->vol, dp->lbasecs, dp->bytes, dp->cap);
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
	int count, loop, cmdport;

	cmdport = cp->cmdport;
	cause = inb(cmdport+Pcount) & 0x03;
	DPRINT("%s: atapiintr 0x%uX\n", cp->dp->vol, cause);
	switch(cause){

	case 1:						/* command */
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cmdport+Perror);
			wakeup(&cp->r); 
			break;
		}
		outss(cmdport+Pdata, cp->cmdblk, sizeof(cp->cmdblk)/2);
		break;

	case 0:						/* data out */
	case 2:						/* data in */
		if(cp->buf == 0){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			if(cp->status & Serr)
				cp->error = inb(cmdport+Perror);
			wakeup(&cp->r);	 
			break;	
		}
		loop = 0;
		while((cp->status & (Serr|Sdrq)) == 0){
			if(++loop > Maxloop){
				cp->status |= Serr;
				break;
			}
			microdelay(1);
			cp->status = inb(cmdport+Pstatus);
		}
		if(cp->status & Serr){
			cp->lastcmd = cp->cmd;
			cp->cmd = 0;
			cp->error = inb(cmdport+Perror);
			print("%s: Cpktcmd status=0x%uX, error=0x%uX\n",
				cp->dp->vol, cp->status, cp->error);
			wakeup(&cp->r);
			break;
		}
		count = inb(cmdport+Pcyllsb)|(inb(cmdport+Pcylmsb)<<8);
		if(cp->count+count > Maxxfer)
			panic("hd%d: count %d, already %d\n", count, cp->count);
		if(cause == 0)
			outss(cmdport+Pdata, cp->buf+cp->count, count/2);
		else
			inss(cmdport+Pdata, cp->buf+cp->count, count/2);
		cp->count += count;
		break;

	case 3:						/* status */
		cp->lastcmd = cp->cmd;
		cp->cmd = 0;
		if(cp->status & Serr)
			cp->error = inb(cp->cmdport+Perror);
		wakeup(&cp->r);	
		break;
	}
}

Dev atadevtab = {
	'H',
	"ata",

	atareset,
	devinit,
	ataattach,
	devclone,
	atawalk,
	atastat,
	ataopen,
	devcreate,
	ataclose,
	ataread,
	devbread,
	atawrite,
	devbwrite,
	devremove,
	devwstat,
};
