#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"io.h"

int	scsiintr(void);

#define	DPRINT	if(debug)kprint

enum {
	Qdir, Qcmd, Qdata, Qdebug,
};

static Dirtab scsidir[]={
	"cmd",		{Qcmd},		0,	0600,
	"data",		{Qdata},	0,	0600,
	"debug",	{Qdebug},	1,	0600,
};

#define	NSCSI	(sizeof scsidir/sizeof(Dirtab))

static Scsi	staticcmd;	/* BUG */
static uchar	datablk[8192];	/* BUG */

static int	debugs[8];
static int	isscsi;
static int	ownid = 0x08|7; /* enable advanced features */

static int
scsigen1(Chan *c, long qid, Dir *dp)
{
	if (qid == CHDIR)
		devdir(c, (Qid){qid,0}, ".", 0, 0500, dp);
	else if (qid == 1)
		devdir(c, (Qid){qid,0}, "id", 1, 0600, dp);
	else if (qid&CHDIR) {
		char name[2];
		name[0] = '0'+((qid>>4)&7), name[1] = 0;
		devdir(c, (Qid){qid,0}, name, 0, 0500, dp);
	} else {
		Dirtab *tab = &scsidir[(qid&7)-1];
		devdir(c, (Qid){qid,0}, tab->name, tab->length, tab->perm, dp);
	}
	return 1;
}

static int
scsigeno(Chan *c, Dirtab *tab, long ntab, long s, Dir *dp)
{
	return scsigen1(c, c->qid.path, dp);
}

static int
scsigen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dp)
{
	if (c->qid.path == CHDIR) {
		if (0<=s && s<=7)
			return scsigen1(c, CHDIR|0x100|(s<<4), dp);
		else if (s == 8)
			return scsigen1(c, 1, dp);
		else
			return -1;
	}
	if (s >= NSCSI)
		return -1;
	return scsigen1(c, (c->qid.path&~CHDIR)+s+1, dp);
}

void
scsireset(void)
{
	addportintr(scsiintr);
}

Chan *
scsiattach(char *param)
{
	return devattach('S', param);
}

Chan *
scsiclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
scsiwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, scsigen);
}

Chan*
scsiclwalk(Chan *c, char *name)
{
	return devclwalk(c, name);
}

void
scsistat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, scsigen);
}

Chan *
scsiopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, scsigeno);
}

void
scsicreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
scsiclose(Chan *c)
{}

long
scsiread(Chan *c, char *a, long n, ulong offset)
{
	Scsi *cmd = &staticcmd;
	if (n == 0)
		return 0;
	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, scsigen);
	if(c->qid.path==1){
		if(offset == 0){
			*a = ownid;
			n = 1;
		}else
			n = 0;
	}else switch((int)(c->qid.path & 0xf)){
	case Qcmd:
		if (n < 4)
			error(Ebadarg);
		/*if(canqlock(cmd)){
			qunlock(cmd);
			error(Egreg);
		}*/
		n = 4;
		*a++ = cmd->state>>8; *a++ = cmd->state;
		*a++ = cmd->status>>8; *a = cmd->status;
		/*qunlock(cmd);*/
		break;
	case Qdata:
		if (n > sizeof datablk)
			error(Ebadarg);
		cmd->data.base = datablk;
		cmd->data.lim = cmd->data.base + n;
		cmd->data.ptr = cmd->data.base;
		cmd->save = cmd->data.base;
		scsiexec(cmd, 1);
		n = cmd->data.ptr - cmd->data.base;
		memmove(a, cmd->data.base, n);
		break;
	case Qdebug:
		if (offset == 0) {
			n=1;
			*a="01"[debugs[(c->qid.path>>4)&7]!=0];
		} else
			n = 0;
		break;
	default:
		panic("scsiread");
	}
	return n;
}

long
scsiwrite(Chan *c, char *a, long n, ulong offset)
{
	Scsi *cmd = &staticcmd;
	if(c->qid.path==1 && n>0){
		if(offset == 0){
			n = 1;
			ownid=*a;
			scsiinit();
		}else
			n = 0;
	}else switch ((int)c->qid.path & 0xf){
	case Qcmd:
		if (n < 6 || n > sizeof cmd->cmdblk)
			error(Ebadarg);
		/*qlock(cmd);*/
		cmd->cmd.base = cmd->cmdblk;
		memmove(cmd->cmd.base, a, n);
		cmd->cmd.lim = cmd->cmd.base + n;
		cmd->cmd.ptr = cmd->cmd.base;
		cmd->target = (c->qid.path>>4)&7;
		cmd->lun = (a[1]>>5)&7;
		cmd->state = 0;
		cmd->status = 0xFFFF;
		break;
	case Qdata:
		if (n > sizeof datablk)
			error(Ebadarg);
		cmd->data.base = datablk;
		cmd->data.lim = cmd->data.base + n;
		cmd->data.ptr = cmd->data.base;
		cmd->save = cmd->data.base;
		memmove(cmd->data.base, a, n);
		scsiexec(cmd, 0);
		n = cmd->data.ptr - cmd->data.base;
		break;
	case Qdebug:
		if (offset == 0) {
			debugs[(c->qid.path>>4)&7] = (*a=='1');
			n = 1;
		} else
			n = 0;
		break;
	default:
		panic("scsiwrite");
	}
	return n;
}

void
scsiremove(Chan *c)
{
	error(Eperm);
}

void
scsiwstat(Chan *c, char *dp)
{
	error(Eperm);
}

void
scsicmd(Scsi *cmd, int dev, int cmdbyte, uchar *buf, long size)
{
	qlock(cmd);
	cmd->target = dev>>3;
	cmd->lun = dev&7;
	cmd->cmd.base = cmd->cmdblk;
	cmd->data.base = buf;
	cmd->cmd.ptr = cmd->cmd.base;
	memset(cmd->cmdblk, 0, sizeof cmd->cmdblk);
	cmd->cmdblk[0] = cmdbyte;
	switch (cmdbyte>>5) {
	case 0:
		cmd->cmd.lim = &cmd->cmdblk[6]; break;
	case 1:
		cmd->cmd.lim = &cmd->cmdblk[10]; break;
	default:
		cmd->cmd.lim = &cmd->cmdblk[12]; break;
	}
	switch (cmdbyte) {
	case 0x00:	/* test unit ready */
		break;
	case 0x03:	/* read sense data */
		cmd->cmdblk[4] = size;
		break;
	case 0x25:	/* read capacity */
		break;
	}
	cmd->data.lim = cmd->data.base + size;
	cmd->data.ptr = cmd->data.base;
	cmd->save = cmd->data.base;
}

int
scsiready(int dev)
{
	static Scsi cmd;
	int status;
	scsicmd(&cmd, dev, 0x00, 0, 0);
	status = scsiexec(&cmd, 0);
	qunlock(&cmd);
	if ((status&0xff00) != 0x6000)
		error(Eio);
	return status&0xff;
}

int
scsisense(int dev, uchar *p)
{
	static Scsi cmd;
	static uchar buf[18];
	int status;
	scsicmd(&cmd, dev, 0x03, buf, sizeof buf);
	status = scsiexec(&cmd, 1);
	memmove(p, buf, sizeof buf);
	qunlock(&cmd);
	if ((status&0xff00) != 0x6000)
		error(Eio);
	return status&0xff;
}

int
scsicap(int dev, uchar *p)
{
	static Scsi cmd;
	static uchar buf[8];
	int status;
	scsicmd(&cmd, dev, 0x25, buf, sizeof buf);
	status = scsiexec(&cmd, 1);
	memmove(p, buf, sizeof buf);
	qunlock(&cmd);
	if ((status&0xff00) != 0x6000)
		error(Eio);
	return status&0xff;
}

typedef struct Scsictl {
	uchar	asr;
	uchar	data;
	uchar	stat;
	uchar	dma;
} Scsictl;

#define	Scsiaddr	48
#define	DEV	((Scsictl *)&PORT[Scsiaddr])

static long	poot;
#define	WAIT	(poot=0, poot==0?0:poot)

#define	PUT(a,d)	(DEV->asr=(a), WAIT, DEV->data=(d))
#define	GET(a)		(DEV->asr=(a), WAIT, DEV->data)

enum Int_status {
	Inten = 0x01, Scsirst = 0x02,
	INTRQ = 0x01, DMA = 0x02,
};

enum SBIC_regs {
	Own_id=0x00, Control=0x01, CDB=0x03, Target_LUN=0x0f,
	Cmd_phase=0x10, Tc_hi=0x12,
	Dest_id=0x15, Src_id=0x16, SCSI_Status=0x17,
	Cmd=0x18, Data=0x19,
};

enum Commands {
	Reset = 0x00,
	Assert_ATN = 0x02,
	Negate_ACK = 0x03,
	Select_with_ATN = 0x06,
	Select_with_ATN_and_Xfr = 0x08,
	Select_and_Xfr = 0x09,
	Transfer_Info = 0x20,
	SBT = 0x80,		/* modifier for single-byte transfer */
};

enum Aux_status {
	INT=0x80, LCI=0x40, BSY=0x20, CIP=0x10,
	PE=0x02, DBR=0x01,
};

static QLock	scsilock;
static Rendez	scsirendez;
static uchar	*datap;
static long	debug, scsirflag, scsibusy, scsiinservice;

static void
nop(void)
{}

static int
scsidone(void *arg)
{
	return (scsibusy == 0);
}

int
scsiexec(Scsi *p, int rflag)
{
	long n;
	debug = debugs[p->target&7];
	DPRINT("scsi %d.%d ", p->target, p->lun);
	qlock(&scsilock);
	if(waserror()){
		qunlock(&scsilock);
		nexterror();
	}
	scsirflag = rflag;
	datap = p->data.base;
	if ((ownid & 0x08) && rflag)
		PUT(Dest_id, 0x40|p->target);
	else
		PUT(Dest_id, p->target);
	PUT(Target_LUN, p->lun);
	n = p->data.lim - p->data.base;
	PUT(Tc_hi, n>>16);
	DEV->data = n>>8;
	DEV->data = n;
	if (ownid & 0x08) {
		n = p->cmd.lim - p->cmd.ptr;
		DPRINT("len=%d ", n);
		PUT(Own_id, n);
	}
	PUT(CDB, *(p->cmd.ptr)++);
	while (p->cmd.ptr < p->cmd.lim)
		DEV->data = *(p->cmd.ptr)++;
	scsibusy = 1;
	PUT(Cmd, Select_and_Xfr);
	DPRINT("S<");
	sleep(&scsirendez, scsidone, 0);
	DPRINT(">");
	p->data.ptr = datap;
	p->status = GET(Target_LUN);
	p->status |= DEV->data<<8;
	poperror();
	qunlock(&scsilock);
	debug = 0;
	return p->status;
}

void
scsirun(void)
{
	wakeup(&scsirendez);
	scsibusy = 0;
}

void
scsiinit(void)
{
	isscsi = portprobe("scsi", -1, Scsiaddr, -1, 0L);
	if (isscsi >= 0) {
		DEV->stat = Scsirst;
		WAIT; nop(); WAIT;
		DEV->stat = Inten;
		while (DEV->stat & (INTRQ|DMA))
			nop();
		ownid &= 0x0f; /* possibly advanced features */
		ownid |= 0x80; /* 16MHz */
		PUT(Own_id, ownid);
		PUT(Cmd, Reset);
	}
}

void
scsireset0(void)
{
	PUT(Control, 0x29);	/* burst DMA, halt on parity error */
	PUT(Control+1, 0xff);	/* timeout */
	PUT(Src_id, 0x80);	/* enable reselection */
	scsirun();
	/*qunlock(&scsilock);*/
}

int
scsiintr(void)
{
	int status, s;
	if (isscsi < 0 || scsiinservice
		|| !((status = DEV->stat) & (DMA|INTRQ)))
			return 0;
	DEV->stat = 0;
	scsiinservice = 1;
	s = spl1();
	DPRINT("i%x ", status);
	do {
		if (status & DMA)
			scsidmaintr();
		if (status & INTRQ)
			scsictrlintr();
	} while ((status = DEV->stat) & (DMA|INTRQ));
	splx(s);
	scsiinservice = 0;
	DEV->stat = Inten;
	return 1;
}

void
scsidmaintr(void)
{
		uchar *p = 0;
/*
 *	if (scsirflag) {
 *		unsigned char *p;
 *		DPRINT("R", p=datap);
 *		do
 *			*datap++ = DEV->dma;
 *		while (DEV->stat & DMA);
 *		DPRINT("%d ", datap-p);
 *	} else {
 *		unsigned char *p;
 *		DPRINT("W", p=datap);
 *		do
 *			DEV->dma = *datap++;
 *		while (DEV->stat & DMA);
 *		DPRINT("%d ", datap-p);
 *	}
 */
	if (scsirflag) {
		DPRINT("R", p=datap);
		datap = scsirecv(datap);
		DPRINT("%d ", datap-p);
	} else {
		DPRINT("X", p=datap);
		datap = scsixmit(datap);
		DPRINT("%d ", datap-p);
	}
}

void
scsictrlintr(void)
{
	int status;
	status = GET(SCSI_Status);
	DPRINT("I%2.2x ", status);
	switch(status){
	case 0x00:			/* reset by command or power-up */
	case 0x01:			/* reset by command or power-up */
		scsireset0();
		break;
	case 0x21:			/* Save Data Pointers message received */
		break;
	case 0x16:			/* select-and-transfer completed */
	case 0x42:			/* timeout during select */
		scsirun();
		break;
	case 0x4b:			/* unexpected status phase */
		PUT(Tc_hi, 0);
		DEV->data = 0;
		DEV->data = 0;
		PUT(Cmd_phase, 0x46);
		PUT(Cmd, Select_and_Xfr);
		break;
	default:
		kprint("scsintr 0x%ux\n", status);
		DEV->asr = Target_LUN;
		kprint("lun/status 0x%ux\n", DEV->data);
		kprint("phase 0x%ux\n", DEV->data);
		switch (status&0xf0) {
		case 0x00:
		case 0x10:
		case 0x20:
		case 0x40:
		case 0x80:
			if (status & 0x08) {
				kprint("count 0x%ux", GET(Tc_hi));
				kprint(" 0x%ux", DEV->data);
				kprint(" 0x%ux\n", DEV->data);
			}
			scsirun();
			break;
		default:
			panic("scsi status 0x%2.2ux", status);
		}
	}
}
