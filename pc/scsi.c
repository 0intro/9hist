#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

enum
{
	Ninq		= 255,
	Nscratch	= 255,

	CMDtest		= 0x00,
	CMDreqsense	= 0x03,
	CMDread6	= 0x08,
	CMDwrite6	= 0x0A,
	CMDinquire	= 0x12,
	CMDstart	= 0x1B,
	CMDread10	= 0x28,
	CMDwrite10	= 0x2A,
};

typedef struct
{
	ISAConf;
	Scsiio	io;

	Target	target[NTarget];
} Ctlr;

static	Ctlr*	scsi[MaxScsi];

typedef struct Link Link;
typedef struct Link
{
	char	*type;
	Scsiio	(*reset)(int, ISAConf*);

	Link*	link;
} Link;

static Link *link;
static int linkcount;

void
addscsilink(char *t, Scsiio (*r)(int, ISAConf*))
{
	Link *lp;

	if((lp = xalloc(sizeof(Link))) == 0)
		return;
	lp->type = t;
	lp->reset = r;

	lp->link = link;
	link = lp;
	linkcount++;
}

void
scsireset(void)
{
	Ctlr *ctlr;
	int ctlrno, t;
	Link *lp;

	for(ctlr = 0, ctlrno = 0; ctlrno < MaxScsi; ctlrno++){
		if(ctlr == 0)
			ctlr = malloc(sizeof(Ctlr));
		memset(ctlr, 0, sizeof(Ctlr));
		if(isaconfig("scsi", ctlrno, ctlr) == 0)
			continue;
		for(lp = link; lp; lp = lp->link){
			if(strcmp(lp->type, ctlr->type))
				continue;
			if((ctlr->io = (*lp->reset)(ctlrno, ctlr)) == 0)
				break;

			print("scsi#%d: %s: port 0x%luX irq %d",
				ctlrno, ctlr->type, ctlr->port,
				ctlr->irq, ctlr->mem, ctlr->size);
			if(ctlr->mem)
				print(" addr 0x%luX", ctlr->mem & ~KZERO);
			if(ctlr->size)
				print(" size 0x%luX", ctlr->size);
			print("\n");

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
scsiexec(Target *t, int rw, uchar *cmd, int cbytes, void *data, int *dbytes)
{
	int s;

	/*
	 * Call the device-specific I/O routine.
	 * There should be no calls to 'error()' below this
	 * which percolate back up.
	 */
	switch(s = (*scsi[t->ctlrno]->io)(t, rw, cmd, cbytes, data, dbytes)){

	default:
		/*
		 * It's more complicated than this. There are conditions which
		 * are 'ok' but for which the returned status code is not 'STok'.
		 * Also, not all conditions require a reqsense, there may be a
		 * need to do a reqsense here when necessary and making it
		 * available to the caller somehow.
		 *
		 * Later.
		 */
		break;
	}

	return s;
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
	Target *t;
	int i, s, nbytes;

	for(i = 0; i < NTarget; i++) {
		t = &ctlr->target[i];

		if(scsitest(t, 0) < 0)
			continue;

		/*
		 * Determine if the drive exists and is not ready or
		 * is simply not responding
		 */
		nbytes = Nscratch;
		s = scsireqsense(t, 0, t->scratch, &nbytes, 0);
		if(s != STok){
			print("scsi#%d: unit %d unavailable, status %d\n", t->ctlrno, i, s);
			continue;
		}

		/*
		 * Inquire to find out what the device is.
		 * Drivers then use the result to attach to targets
		 */
		memset(t->inq, 0, Ninq);
		nbytes = Ninq;
		s = scsiinquiry(t, 0, t->inq, &nbytes);
		if(s != STok) {
			print("scsi#%d: unit %d inquire failed, status %d\n", t->ctlrno, i, s);
			continue;
		}
		print("scsi#%d: unit %d: %s\n", t->ctlrno, i, t->inq+8);
		t->ok = 1;
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
scsiinv(int devno, int *type, Target **rt, uchar **inq, char *id)
{
	Target *t;
	int ctlr, *i, unit;

	inventory();

	for(;;){
		ctlr = devno/NTarget;
		unit = devno%NTarget;
		if(ctlr >= MaxScsi || scsi[ctlr] == 0)
			return -1;

		t = &scsi[ctlr]->target[unit];
		devno++;
		if(t->ok){
			for(i = type; *i >= 0; i++){
				if((t->inq[0]&0x1F) != *i)
					continue;
				*rt = t;
				*inq = t->inq;
				if(id)
					sprint(id, "scsi%d: unit %d", ctlr, unit);
				return devno;
			}
		}
	}
	return -1;
}


int
scsitest(Target *t, char lun)
{
	uchar cmd[6];

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = CMDtest;
	cmd[1] = lun<<5;
	return scsiexec(t, SCSIread, cmd, sizeof(cmd), 0, 0);
}

int
scsistart(Target *t, char lun, int s)
{
	uchar cmd[6];

	memset(cmd, 0, sizeof cmd);
	cmd[0] = CMDstart;
	cmd[1] = lun<<5;
	cmd[4] = s? 1: 0;
	return scsiexec(t, SCSIread, cmd, sizeof(cmd), 0, 0);
}

int
scsiinquiry(Target *t, char lun, void *data, int *datalen)
{
	uchar cmd[6];

	memset(cmd, 0, sizeof cmd);
	cmd[0] = CMDinquire;
	cmd[1] = lun<<5;
	cmd[4] = *datalen;
	return scsiexec(t, SCSIread, cmd, sizeof(cmd), data, datalen);
}

int
scsicap(Target *t, char lun, ulong *size, ulong *bsize)
{
	int s, nbytes;
	uchar cmd[10], *d;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x25;
	cmd[1] = lun<<5;

	nbytes = 8;
	d = scsialloc(nbytes);
	if(d == 0)
		return -1;

	s = scsiexec(t, SCSIread, cmd, sizeof(cmd), d, &nbytes);
	if(s == STok) {
		*size  = nhgetl(d+0);
		*bsize = nhgetl(d+4);
	}
	scsifree(d);
	return s;
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
		hnputs(cmd+2, bno);
		cmd[4] = n;
		cdbsiz = 6;
	}
	else {
		cmd[0] = 0x2A;
		if(dir == SCSIread)
			cmd[0] = 0x28;
		cmd[1] = (lun<<5);
		hnputl(cmd+2, bno);
		hnputs(cmd+7, n);
		cdbsiz = 10;
	}
	nbytes = n*bsize;
	s = scsiexec(t, dir, cmd, cdbsiz, b, &nbytes);
	if(s != STok) {
		nbytes = Nscratch;
		scsireqsense(t, lun, t->scratch, &nbytes, 0);
		return scsierrstr(s);
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
scsireqsense(Target *t, char lun, void *data, int *nbytes, int quiet)
{
	char *s;
	int status, try;
	uchar cmd[6], *sense;

	sense = malloc(*nbytes);

	for(try = 0; try < 20; try++) {
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = CMDreqsense;
		cmd[1] = lun<<5;
		cmd[4] = *nbytes;
		memset(sense, 0, *nbytes);

		status = scsiexec(t, SCSIread, cmd, sizeof(cmd), sense, nbytes);
		if(status != STok){
			free(sense);
			return status;
		}
		*nbytes = sense[0x07]+8;
		memmove(data, sense, *nbytes);

		switch(sense[2] & 0x0F){

		case 6:						/* unit attention */
			if(sense[12] != 0x29)			/* power on, reset */
				goto buggery;
			/*FALLTHROUGH*/
		case 0:						/* no sense */
		case 1:						/* recovered error */
			free(sense);
			return STok;

		case 2:						/* not ready */
			if(sense[12] == 0x3A)			/* medium not present */
				goto buggery;
			/*FALLTHROUGH*/

		default:
			/*
			 * If unit is becoming ready, rather than not ready,
			 * then wait a little then poke it again; should this
			 * be here or in the caller?
			 */
			if((sense[12] == 0x04 && sense[13] == 0x01)){
				while(waserror())
					;
				tsleep(&t->rendez, return0, 0, 500);
				poperror();
				scsitest(t, lun);
				break;
			}
			goto buggery;
		}
	}

buggery:
	if(quiet == 0){
		s = key[sense[2]&0x0F];
		print("scsi#%d: unit %d reqsense: '%s' code #%2.2ux #%2.2ux\n",
			t->ctlrno, t->target, s, sense[12], sense[13]);
	}
	free(sense);
	return STcheck;
}

int
scsidiskinfo(Target *t, char lun, int track, uchar *data)
{
	int s, nbytes;
	uchar cmd[10], *d;

	nbytes = 12;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x43;
	cmd[1] = lun<<5;
	cmd[6] = track;
	hnputs(cmd+7, nbytes);

	d = scsialloc(nbytes);
	if(d == 0)
		return scsierrstr(STnomem);

	memset(d, 0, nbytes);
	s = scsiexec(t, SCSIread, cmd, sizeof(cmd), d, &nbytes);
	memmove(data, d, 12);
	scsifree(d);
	return s;
}

int
scsitrackinfo(Target *t, char lun, int track, uchar *data)
{
	int s, nbytes;
	uchar cmd[10], *d;

	nbytes = 12;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0xe5;
	cmd[1] = lun<<5;
	cmd[5] = track;
	hnputs(cmd+7, nbytes);

	d = scsialloc(nbytes);
	if(d == 0)
		return scsierrstr(STnomem);

	memset(d, 0, nbytes);
	s = scsiexec(t, SCSIread, cmd, sizeof(cmd), d, &nbytes);
	memmove(data, d, 12);
	scsifree(d);

	return s;
}

int
scsibufsize(Target *t, char lun, int size)
{
	int s, nbytes;
	uchar cmd[6], *d;


	nbytes = 12;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x15;
	cmd[1] = lun<<5;
	cmd[4] = nbytes;

	d = scsialloc(nbytes);
	if(d == 0)
		return scsierrstr(STnomem);

	memset(d, 0, nbytes);
	d[3] = 8;
	hnputl(d+8, size);

	s = scsiexec(t, SCSIwrite, cmd, sizeof(cmd), d, &nbytes);
	scsifree(d);
	return s;
}

int
scsireadcdda(Target *t, char lun, int, void *b, long n, long bsize, long bno)
{
	uchar cmd[10];
	int s, nbytes;

	memset(cmd, 0, sizeof(cmd));

	cmd[0] = 0xd8;
	cmd[1] = (lun<<5);
	hnputl(cmd+2, bno);
	hnputl(cmd+6, n);

	nbytes = n*bsize;
	s = scsiexec(t, SCSIread, cmd, sizeof(cmd), b, &nbytes);
	if(s != STok) {
		nbytes = Nscratch;
		scsireqsense(t, lun, t->scratch, &nbytes, 0);
		return scsierrstr(s);
	}
	return nbytes;
}

int
scsierrstr(int errno)
{
	char *p;

	switch(errno){
	case STnomem:
		p = Enomem;
		break;
	case STtimeout:
		p = "bus timeout";
		break;
	case STownid:
		p = "playing with myself";
		break;
	case STharderr:
		p = Eio;
		break;
	case STok:
		p = Enoerror;
		break;
	case STcheck:
		p = "check condition";
		break;
	case STcondmet:
		p = "condition met/good";
		break;
	case STbusy:
		p = "busy";
		break;
	case STintok:
		p = "intermediate/good";
		break;
	case STintcondmet:
		p = "intermediate/condition met/good";
		break;
	case STresconf:
		p = "reservation conflict";
		break;
	case STterminated:
		p = "command terminated";
		break;
	case STqfull:
		p = "queue full";
		break;

	default:
		p = "unknown SCSI error";
		break;
	}
	strncpy(up->error, p, NAMELEN);

	return -1;
}
