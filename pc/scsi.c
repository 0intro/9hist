#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"
#include "../port/netif.h"

enum {
	Ninq		= 255,
	Nscratch	= 255,

	CMDreqsense	= 0x03,
	CMDinquire	= 0x12,
};

typedef struct {
	ISAConf;
	Scsiio	io;

	Target	target[NTarget];
} Ctlr;

static Ctlr *scsi[MaxScsi];

static struct {
	char	*type;
	Scsiio	(*reset)(int, ISAConf*);
} cards[MaxScsi+1];

void
addscsicard(char *t, Scsiio (*r)(int, ISAConf*))
{
	static int ncard;

	if(ncard == MaxScsi)
		panic("too many scsi cards\n");
	cards[ncard].type = t;
	cards[ncard].reset = r;
	ncard++;
}

void
scsireset(void)
{
	Ctlr *ctlr;
	int ctlrno, n, t;

	for(ctlr = 0, ctlrno = 0; ctlrno < MaxScsi; ctlrno++){
		if(ctlr == 0)
			ctlr = malloc(sizeof(Ctlr));
		memset(ctlr, 0, sizeof(Ctlr));
		if(isaconfig("scsi", ctlrno, ctlr) == 0)
			continue;
		for(n = 0; cards[n].type; n++){
			if(strcmp(cards[n].type, ctlr->type))
				continue;
			if((ctlr->io = (*cards[n].reset)(ctlrno, ctlr)) == 0)
				break;

			print("scsi%d: %s: port %lux irq %d addr %lux size %d\n",
				ctlrno, ctlr->type, ctlr->port,
				ctlr->irq, ctlr->mem, ctlr->size);

			for(t = 0; t < NTarget; t++){
				ctlr->target[t].ctlrno = ctlrno;
				ctlr->target[t].target = t;
				ctlr->target[t].inq = xalloc(Ninq);
				ctlr->target[t].scratch = xalloc(Nscratch);
			}

			scsi[ctlrno] = ctlr;
			ctlr = 0;
			break;
		}
	}
	if(ctlr)
		free(ctlr);
}

int
scsiexec(Target *tp, int rw, uchar *cmd, int cbytes, void *data, int *dbytes)
{
	return (*scsi[tp->ctlrno]->io)(tp, rw, cmd, cbytes, data, dbytes);
}

Target*
scsiunit(int ctlr, int unit)
{
	Target *t;

	if(ctlr < 0 || ctlr >= MaxScsi || scsi[ctlr] == 0)
		return 0;
	if(unit < 0 || unit >= NTarget)
		return 0;
	t = &scsi[ctlr]->target[unit];
	if(t->ok == 0)
		return 0;
	return t;
}

static void
scsiprobe(Ctlr *ctlr)
{
	Target *tp;
	uchar cmd[6];
	int i, s, nbytes;

	for(i = 0; i < NTarget; i++) {
		tp = &ctlr->target[i];

		/*
		 * Test unit ready
		 */
		memset(cmd, 0, sizeof(cmd));
		s = scsiexec(tp, SCSIread, cmd, sizeof(cmd), 0, 0);
		if(s < 0)
			continue;

		/*
		 * Determine if the drive exists and is not ready or
		 * is simply not responding
		 */
		if((s = scsireqsense(tp, 0, 0)) != STok){
			print("scsi%d: unit %d unavailable, status %d\n", tp->ctlrno, i, s);
			continue;
		}

		/*
		 * Inquire to find out what the device is
		 * Drivers then use the result to attach to targets
		 */
		memset(tp->inq, 0, Ninq);
		cmd[0] = CMDinquire;
		cmd[4] = Ninq;
		nbytes = Ninq;
		s = scsiexec(tp, SCSIread, cmd, sizeof(cmd), tp->inq, &nbytes);
		if(s < 0) {
			print("scsi%d: unit %d inquire failed, status %d\n", tp->ctlrno, i, s);
			continue;
		}
		print("scsi%d: unit %d %s\n", tp->ctlrno, i, tp->inq+8);
		tp->ok = 1;
	}
}

static void
inventory(void)
{
	int i;
	static Lock ilock;
	static int inited;

	lock(&ilock);
	if(inited) {
		unlock(&ilock);
		return;
	}
	inited = 1;
	unlock(&ilock);

	for(i = 0; i < MaxScsi; i++){
		if(scsi[i])
			scsiprobe(scsi[i]);
	}
}

int
scsiinv(int devno, int type, Target **rt, uchar **inq, char *id)
{
	Target *t;
	int ctlr, unit;

	inventory();

	for(;;){
		ctlr = devno/NTarget;
		unit = devno%NTarget;
		if(ctlr >= MaxScsi || scsi[ctlr] == 0)
			return -1;

		t = &scsi[ctlr]->target[unit];
		devno++;
		if(t->ok && (t->inq[0]&0x0F) == type){
			*rt = t;
			*inq = t->inq;
			sprint(id, "scsi%d: unit %d", ctlr, unit);
			return devno;
		}
	}
	return -1;
}

int
scsistart(Target *t, char lun, int s)
{
	uchar cmd[6];

	memset(cmd, 0, sizeof cmd);
	cmd[0] = 0x1b;
	cmd[1] = lun<<5;
	cmd[4] = s ? 1 : 0;
	return scsiexec(t, SCSIread, cmd, sizeof(cmd), 0, 0);
}

int
scsicap(Target *t, char lun, ulong *size, ulong *bsize)
{
	int s, nbytes;
	uchar cmd[10], *d;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x25;
	cmd[1] = lun<<5;

	d = malloc(8);
	if(d == 0)
		return -1;

	nbytes = 8;
	s = scsiexec(t, SCSIread, cmd, sizeof(cmd), d, &nbytes);
	if(s < 0) {
		free(d);
		return s;
	}
	*size  = (d[0]<<24)|(d[1]<<16)|(d[2]<<8)|(d[3]<<0);
	*bsize = (d[4]<<24)|(d[5]<<16)|(d[6]<<8)|(d[7]<<0);
	free(d);
	return 0;
}

int
scsibio(Target *t, char lun, int dir, void *b, long n, long bsize, long bno)
{
	uchar cmd[10];
	int s, cdbsiz, nbytes;

	memset(cmd, 0, sizeof cmd);
	if(bno <= 0x1fffff && n < 256) {
		cmd[0] = 0x0A;
		if(dir == SCSIread)
			cmd[0] = 0x08;
		cmd[1] = (lun<<5) | bno >> 16;
		cmd[2] = bno >> 8;
		cmd[3] = bno;
		cmd[4] = n;
		cdbsiz = 6;
	}
	else {
		cmd[0] = 0x2A;
		if(dir == SCSIread)
			cmd[0] = 0x28;
		cmd[1] = (lun<<5);
		cmd[2] = bno >> 24;
		cmd[3] = bno >> 16;
		cmd[4] = bno >> 8;
		cmd[5] = bno;
		cmd[7] = n>>8;
		cmd[8] = n;
		cdbsiz = 10;
	}
	nbytes = n*bsize;
	s = scsiexec(t, dir, cmd, cdbsiz, b, &nbytes);
	if(s < 0) {
		scsireqsense(t, lun, 0);
		return -1;
	}
	return nbytes;
}

static char *key[] =
{
	"no sense",
	"recovered error",
	"not ready",
	"medium error",
	"hardware error",
	"illegal request",
	"unit attention",
	"data protect",
	"blank check",
	"vendor specific",
	"copy aborted",
	"aborted command",
	"equal",
	"volume overflow",
	"miscompare",
	"reserved"
};

int
scsireqsense(Target *tp, char lun, int quiet)
{
	char *s;
	int sr, try, nbytes;
	uchar cmd[6], *sense;

	sense = tp->scratch;

	for(try = 0; try < 5; try++) {
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = CMDreqsense;
		cmd[1] = lun<<5;
		cmd[4] = Nscratch;
		memset(sense, 0, sizeof(sense));

		nbytes = Nscratch;
		sr = scsiexec(tp, SCSIread, cmd, sizeof(cmd), sense, &nbytes);
		if(sr != STok)
			return sr;

		/*
		 * Unit attention. We can handle that.
		 */
		if((sense[2] & 0x0F) == 0x00 || (sense[2] & 0x0F) == 0x06)
			return STok;

		/*
		 * Recovered error. Why bother telling me.
		 */
		if((sense[2] & 0x0F) == 0x01)
			return STok;

		/*
		 * Unit is becoming ready
		 */
		if(sense[12] != 0x04 || sense[13] != 0x01)
			break;

		delay(5000);
	}

	if(quiet)
		return STcheck;

	s = key[sense[2]&0xf];
	print("scsi%d: unit %d reqsense: '%s' code #%2.2ux #%2.2ux\n",
		tp->ctlrno, tp->target, s, sense[12], sense[13]);
	return STcheck;
}

