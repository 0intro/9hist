#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#define NOW (MACHP(0)->ticks)
#define DPRINT if(0)

enum {
	/*
	 *  configuration parameters
	 */
	Ndk = 2,		/* max dks */

	/*
	 *  relative or immutable
	 */
	Nline = 256,		/* max lines per dk */
	Ndir = Nline + 1,	/* entries in the dk directory */
	Nsubdir = 5,		/* entries in the sub directory */
};

typedef struct Dkmsg	Dkmsg;
typedef struct Line	Line;
typedef struct Dk	Dk;

/*
 *  types of possible dkcalls
 */
enum {
	Dial,
	Announce,
	Redial
};

/*
 *  format of messages to/from the datakit controller on the common
 *  signalling line
 */
struct Dkmsg {
	uchar	type;
	uchar	srv;
	uchar	param0l;
	uchar	param0h;
	uchar	param1l;
	uchar	param1h;
	uchar	param2l;
	uchar	param2h;
	uchar	param3l;
	uchar	param3h;
	uchar	param4l;
	uchar	param4h;
};

/*
 *  message codes (T_xxx == dialin.type, D_xxx == dialin.srv)
 */
#define	T_SRV	1		/* service request */
#define   D_SERV	1		/* (host to dkmux) announce a service */
#define   D_DIAL	2		/* (host to dkmux) connect to a service */
#define   D_XINIT	7		/* (dkmux to host) line has been spliced */
#define	T_REPLY	2		/* reply to T_SRV/D_SERV or T_SRV/D_DIAL */
#define	  D_OK		1		/* not used */
#define	  D_OPEN	2		/* (dkmux to host) connection established */
#define	  D_FAIL	3		/* (dkmux to host) connection failed */
#define	T_CHG	3		/* linege the status of a connection */
#define	  D_CLOSE	1		/* close the connection */
#define	  D_ISCLOSED	2		/* (dkmux to host) confirm a close */
#define	  D_CLOSEALL	3		/* (dkmux to host) close all connections */
#define	  D_REDIAL	6		/* (host to dkmux) redial a call */
#define	T_ALIVE	4		/* (host to dkmux) keep alive message */
#define	  D_CONTINUE	0		/* host has not died since last msg */
#define	  D_RESTART	1		/* host has restarted */
#define   D_MAXCHAN	2		/* request maximum line number */
#define	T_RESTART 8		/* (dkmux to host) datakit restarted */

/*
 *  macros for cracking/forming the window negotiation parameter
 */
#define MIN(x,y)  (x < y ? x : y)
#define W_WINDOW(o,d,t)  ((o<<8) | (d<<4) | t | 0100000)
#define W_VALID(x)  ((x) & 0100000)
#define W_ORIG(x)  (((x)>>8) & 017)
#define W_DEST(x)  (((x)>>4) & 017)
#define W_TRAF(x)  ((x) & 017)
#define W_DESTMAX(x,y)  (W_WINDOW(W_ORIG(x),MIN(W_DEST(x),y),W_TRAF(x)))
#define W_LIMIT(x,y)  (W_WINDOW(MIN(W_ORIG(x),y),MIN(W_DEST(x),y),W_TRAF(x)))
#define	W_VALUE(x)	(1<<((x)+4))
#define WS_2K	7

/*
 *  one per datakit line
 */
struct Line {
	QLock;
	Rendez	r;		/* wait here for dial */
	int	state;		/* dial state */
	int	err;		/* dialing error (if non zero) */
	int	window;		/* negotiated window */
	int	timestamp;	/* timestamp of last call received on this line */
	int	calltolive;	/* multiple of 15 seconds for dialing state to last */
	Queue	*rq;
	char	addr[64];
	char	raddr[64];
	char	ruser[32];
	char	other[64];
	Dk *dp;			/* interface contianing this line */
};

/*
 *  a dkmux dk.  one exists for every stream that a 
 *  dkmux line discipline is pushed onto.
 */
struct Dk {
	QLock;
	int	ref;
	char	name[64];	/* dk name */	
	Queue	*wq;		/* dk output queue */
	int	lines;		/* number of lines */
	int	ncsc;		/* csc line number */
	Chan	*csc;		/* common signalling line */
	Line	line[Nline];
};
static Dk dk[Ndk];

/*
 *  conversation states (for Line.state)
 */
typedef enum {
	Lclosed=0,
	Lopened,		/* opened but no call out */
	Lconnected,		/* opened and a call set up on htis line */
	Lrclose,		/* remote end has closed down */
	Llclose,		/* local end has closed down */
	Ldialing,		/* dialing a new call */
	Llistening,		/* this line listening for calls */
	Lackwait,		/* incoming call waiting for ack/nak */
	Laccepting,		/* waiting for user to accept or reject the call */
} Lstate;

/*
 *  datakit error to errno 
 */
enum {
	DKok,
	DKbusy,
	DKnetotl,
	DKdestotl,
	DKbadnet,
	DKnetbusy,
	DKinuse,
	DKreject,
};
int dkerr[]={
	[DKok]Egreg,
	[DKbusy]Einuse,		/* destination busy */
	[DKnetotl]Enetotl,	/* network not answering */
	[DKdestotl]Edestotl,	/* destination not answering */ 
	[DKbadnet]Ebadnet,	/* unassigned destination */
	[DKnetbusy]Enetbusy,	/* network overload */
	[DKinuse]Einuse,	/* server already exists */
	[DKreject]Erejected	/* call rejected by destination */
};
#define DKERRS sizeof(dkerr)/sizeof(int)

/*
 *  imported
 */
extern Qinfo urpinfo;

/*
 *  predeclared
 */
Chan*		dkattach(char*);
static void	dkmuxconfig(Dk*, Block*);
static int	dkmesg(Dk*, int, int, int, int);
static void	dkcsckproc(void*);
static int	dklisten(Chan*);
static void	dkanswer(Chan*, int, int);
static void	dkwindow(Chan*);
static void	dkcall(int, Chan*, char*, char*, char*);
static void	dktimer(void*);
static void	dkchgmesg(Dk*, Dkmsg*, int);
static void	dkreplymesg(Dk*, Dkmsg*, int);
Chan*		dkopen(Chan*, int);

/*
 *  the datakit multiplexor stream module definition
 */
static void dkmuxopen(Queue *, Stream *);
static void dkmuxclose(Queue *);
static void dkmuxoput(Queue *, Block *);
static void dkmuxiput(Queue *, Block *);
Qinfo dkmuxinfo = { dkmuxiput, dkmuxoput, dkmuxopen, dkmuxclose, "dkmux" };

/*
 *  a new dkmux.  find a free dk structure and assign it to this queue.
 */
static void
dkmuxopen(Queue *q, Stream *s)
{
	Dk *dp;
	int i;

	for(dp = dk; dp < &dk[Ndk]; dp++){
		if(dp->wq == 0){
			qlock(dp);
			if(dp->wq) {
				/* someone was faster than us */
				qunlock(dp);
				continue;
			}
			q->ptr = q->other->ptr = (void *)dp;
			dp->csc = 0;
			dp->ncsc = 4;
			dp->lines = 16;
			dp->name[0] = 0;
			dp->wq = WR(q);
			qunlock(dp);
			return;
		}
	}
	error(0, Enoifc);
}

/*
 *  close down a dkmux
 */
static void
dkmuxclose(Queue *q)
{
	Dk *dp;

	dp = (Dk *)q->ptr;
	qlock(dp);
	if(dp->csc)
		close(dp->csc);
	dp->wq = 0;
	qunlock(dp);
}

/*
 *  handle configuration
 */
static void
dkmuxoput(Queue *q, Block *bp)
{
	Dk *dp;

	dp = (Dk *)q->ptr;
	if(bp->type != M_DATA){
		if(streamparse("config", bp))
			dkmuxconfig(dp, bp);
		else
			PUTNEXT(q, bp);
		return;
	}
	PUTNEXT(q, bp);
}

/*
 *  gather a message and send it up the appropriate stream
 *
 *  The first two bytes of each message contains the channel
 *  number, low order byte first.
 *
 *  Simplifying assumption:  one put == one message && the channel number
 *	is in the first block.  If this isn't true, demultiplexing will not
  *	work.
 */
static void
dkmuxiput(Queue *q, Block *bp)
{
	Dk *dp;
	Line *lp;
	int line;

	dp = (Dk *)q->ptr;
	if(bp->type != M_DATA){
		PUTNEXT(q, bp);
		return;
	}

	line = bp->rptr[0] | (bp->rptr[1]<<8);
	bp->rptr += 2;
	if(line<0 || line>=dp->lines){
		DPRINT("dkmuxiput bad line %d\n", line);
		freeb(bp);
		return;
	}

	lp = &dp->line[line];
	if(canqlock(lp)){
		if(lp->rq)
			PUTNEXT(lp->rq, bp);
		else{
			DPRINT("dkmuxiput unopened line %d\n", line);
			freeb(bp);
		}
		qunlock(lp);
	} else {
		DPRINT("dkmuxiput unopened line %d\n", line);
		freeb(bp);
	}
}

/*
 *  the datakit line stream module definition
 */
static void dkstopen(Queue *, Stream *);
static void dkstclose(Queue *);
static void dkoput(Queue *, Block *);
static void dkiput(Queue *, Block *);
Qinfo dkinfo = { dkiput, dkoput, dkstopen, dkstclose, "dk" };

/*
 *  open and save a pointer to the conversation
 */
static void
dkstopen(Queue *q, Stream *s)
{
	Dk *dp;
	Line *lp;

	dp = &dk[s->dev];
	q->other->ptr = q->ptr = lp = &dp->line[s->id];
	lp->dp = dp;
	lp->rq = q;
}

/*
 *  close down a datakit conversation
 */
static void
dkstclose(Queue *q)
{
	Dk *dp;
	Line *lp;

	lp = (Line *)q->ptr;
	dp = lp->dp;

	/*
	 *  shake hands with dk
	 */
	switch(lp->state){
	case Lclosed:
	case Llclose:
		break;

	case Lrclose:
		dkmesg(dp, T_CHG, D_CLOSE, lp - dp->line, 0);
		lp->state = Lclosed;
		break;

	case Lackwait:
		dkmesg(dp, T_CHG, D_CLOSE, lp - dp->line, 0);
		lp->state = Llclose;
		break;

	case Llistening:
		dkmesg(dp, T_CHG, D_CLOSE, lp - dp->line, 0);
		lp->state = Llclose;
		break;

	case Lconnected:
		dkmesg(dp, T_CHG, D_CLOSE, lp - dp->line, 0);
		lp->state = Llclose;
		break;

	case Lopened:
		lp->state = Lclosed;
	}

	qlock(lp);
	lp->rq = 0;
	qunlock(lp);
}

/*
 *  this is only called by hangup
 */
static void
dkiput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

/*
 *  we assume that each put is a message.
 *
 *  add a 2 byte channel number to the start of each message
 */
static void
dkoput(Queue *q, Block *bp)
{
	Line *lp;
	Dk *dp;
	int line;

	if(bp->type != M_DATA){
		freeb(bp);
		error(0, Ebadarg);
	}

	lp = (Line *)q->ptr;
	dp = lp->dp;
	line = lp - dp->line;

	if(bp->base && bp->rptr - bp->base >= 2)
		bp->rptr -= 2;
	else
		panic("dkoput");
	bp->rptr[0] = line;
	bp->rptr[1] = line>>8;

	PUTNEXT(dp->wq, bp);
}

/*
 *  configure a datakit multiplexor.  this takes 3 arguments separated
 *  by spaces:
 *	the line number of the common signalling channel (must be > 0)
 *	the number of lines in the device (optional)
 *	the name of the dk (optional)
 *
 *  we can configure only once
 */
static void
dkmuxconfig(Dk *dp, Block *bp)
{
	Chan *c;
	char *fields[3];
	int n;
	char buf[64];
	static int dktimeron;

	if(dp->csc != 0){
		freeb(bp);
		error(0, Ebadarg);
	}

	/*
	 *  parse
	 */
	n = getfields((char *)bp->rptr, fields, 3, ' ');
	switch(n){
	case 3:
		strncpy(dp->name, fields[2], sizeof(dp->name));
	case 2:
		dp->lines = strtoul(fields[1], 0, 0);
	case 1:
		dp->ncsc = strtoul(fields[0], 0, 0);
		break;
	default:
		freeb(bp);
		error(0, Ebadarg);
	}
	freeb(bp);
	if(dp->ncsc <= 0 || dp->lines <= dp->ncsc){
		dp->lines = 16;
		error(0, Ebadarg);
	}

	/*
	 *  open a stream for the csc and push urp onto it
	 */
	c = 0;
	if(waserror()){
		if(c)
			close(c);
		nexterror();
	}
	c = dkattach(dp->name);
	c->qid = STREAMQID(dp->ncsc, Sdataqid);
	dkopen(c, ORDWR);
	dp->csc = c;

	/*
	 *  start a process to deal with it
	 */
	sprint(buf, "**csckproc%d**", dp->ncsc);
	kproc(buf, dkcsckproc, dp);
	poperror();

	/*
	 *  start a keepalive process if one doesn't exist
	 */
	if(dktimeron == 0){
		dktimeron = 1;
		kproc("**dktimer**", dktimer, 0);
	}
}

/*
 *  qid's
 */
enum {
	/*
	 *  per line
	 */
	Daddrqid,
	Dlistenqid,
	Draddrqid,
	Duserqid,
	Dotherqid,
	Dlineqid,

	/*
	 *  per device
	 */
	Dcloneqid,
};

/*
 *  the dk directory
 */
Dirtab dkdir[Ndir];

/*
 *  the per stream directory structure
 */
Dirtab dksubdir[]={
	"addr",		Daddrqid,	0,	0600,
	"listen",	Dlistenqid,	0,	0600,
	"other",	Dotherqid,	0,	0600,
	"raddr",	Draddrqid,	0, 	0600,
	"ruser",	Duserqid,	0, 	0600,
};

/*
 *  dk file system.  most of the calls use dev.c to access the dk
 *  directory and stream.c to access the dk devices.
 */
void
dkreset(void)
{
	newqinfo(&dkmuxinfo);
	newqinfo(&urpinfo);
}

/*
 *  create the dk directory.  the files are `clone' and stream
 *  directories '1' to '32' (or whatever Nline is in decimal)
 */
void
dkinit(void)
{
	int i;

	/*
	 *  create the directory.
	 */
	/*
	 *  the circuits
	 */
	for(i = 1; i < Nline; i++) {
		sprint(dkdir[i].name, "%d", i);
		dkdir[i].qid = CHDIR|STREAMQID(i, Dlineqid);
		dkdir[i].length = 0;
		dkdir[i].perm = 0600;
	}

	/*
	 *  the clone device
	 */
	strcpy(dkdir[0].name, "clone");
	dkdir[0].qid = Dcloneqid;
	dkdir[0].length = 0;
	dkdir[0].perm = 0600;
}

Chan*
dkattach(char *spec)
{
	Chan *c;
	Dk *dp;

	/*
	 *  find a multiplexor with the same name
	 */
	for(dp = dk; dp < &dk[Ndk]; dp++){
		qlock(dp);
		if(dp->wq && strcmp(spec, dp->name)==0) {
			dp->ref++;
			qunlock(dp);
			break;
		}
		qunlock(dp);
	}
	if(dp == &dk[Ndk])
		error(0, Enoifc);
	c = devattach('k', spec);
	c->dev = dp - dk;
	return c;
}

Chan*
dkclone(Chan *c, Chan *nc)
{
	Dk *dp;

	dp = &dk[c->dev];
	qlock(dp);
	dp->ref++;
	qunlock(dp);
	return devclone(c, nc);
}

int	 
dkwalk(Chan *c, char *name)
{
	if(c->qid == CHDIR)
		return devwalk(c, name, dkdir, dk[c->dev].lines, devgen);
	else
		return devwalk(c, name, dksubdir, Nsubdir, streamgen);
}

void	 
dkstat(Chan *c, char *dp)
{
	if(c->qid == CHDIR)
		devstat(c, dp, dkdir, dk[c->dev].lines, devgen);
	else
		devstat(c, dp, dksubdir, Nsubdir, streamgen);
}

/*
 *  opening a dk device allocates a Line.  Opening the `clone'
 *  device is a ``macro'' for finding a free Line and opening
 *  it's ctl file.
 *
 *  opening the `listen' sub device is a macro for listening for
 *  a new call.  Lile `clone' the ctl file of the new channel is
 *  returned.
 */
Chan*
dkopen(Chan *c, int omode)
{
	extern Qinfo dkinfo;
	Stream *s;
	Line *lp, *end;
	Dk *dp;
	int line;

	if(c->qid & CHDIR){
		/*
		 *  directories are read only
		 */
		if(omode != OREAD)
			error(0, Ebadarg);
	} else switch(STREAMTYPE(c->qid)){
	case Dcloneqid:
		/*
		 *  get an unused device and open it's control file
		 */
		dp = &dk[c->dev];
		end = &dp->line[dp->lines];
		for(lp = &dp->line[dp->ncsc+1]; lp < end; lp++){
			if(lp->state == Lclosed && canqlock(lp)){
				if(lp->state != Lclosed){
					qunlock(lp);
					continue;
				}
				c->qid = STREAMQID(lp-dp->line, Sctlqid);
				qunlock(lp);
				break;
			}
		}
		if(lp == end)
			error(0, Enodev);
		streamopen(c, &dkinfo);
		pushq(c->stream, &urpinfo);
		break;
	case Dlistenqid:
		/*
		 *  listen for a call and open the control file for the
		 *  channel on which the call arrived.
		 */
		line = dklisten(c);
		c->qid = STREAMQID(line, Sctlqid);
		streamopen(c, &dkinfo);
		pushq(c->stream, &urpinfo);
		dkwindow(c);
		break;
	case Daddrqid:
	case Draddrqid:
	case Duserqid:
	case Dotherqid:
		/*
		 *  read only files
		 */
		if(omode != OREAD)
			error(0, Ebadarg);
		break;
	default:
		/*
		 *  open whatever c points to, make sure it has an urp
		 */
		streamopen(c, &dkinfo);
		if(strcmp(c->stream->procq->next->info->name, "urp")!=0)
			pushq(c->stream, &urpinfo);
		break;
	}

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
dkcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void	 
dkclose(Chan *c)
{
	Dk *dp;

	/* real closing happens in lancestclose */
	if(c->stream)
		streamclose(c);

	dp = &dk[c->dev];
	qlock(dp);
	dp->ref--;
	qunlock(dp);
}

long	 
dkread(Chan *c, void *a, long n)
{
	Line *lp;

	if(c->stream)
		return streamread(c, a, n);

	if(c->qid & CHDIR){
		if(c->qid == CHDIR)
			return devdirread(c, a, n, dkdir, dk[c->dev].lines, devgen);
		else
			return devdirread(c, a, n, dksubdir, Nsubdir, streamgen);
	}

	lp = &dk[c->dev].line[STREAMID(c->qid)];
	switch(STREAMTYPE(c->qid)){
	case Daddrqid:
		return stringread(c, a, n, lp->addr);
	case Draddrqid:
		return stringread(c, a, n, lp->raddr);
	case Duserqid:
		return stringread(c, a, n, lp->ruser);
	}
	error(0, Eperm);
}

long	 
dkwrite(Chan *c, void *a, long n)
{
	int t;
	char buf[256];
	char *field[5];
	int m;

	t = STREAMTYPE(c->qid);

	/*
	 *  get data dispatched as quickly as possible
	 */
	if(t == Sdataqid)
		return streamwrite(c, a, n, 0);

	/*
	 *  easier to do here than in dkoput
	 */
	if(t == Sctlqid){
		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 5, ' ');
		if(strcmp(field[0], "connect")==0){
			if(m < 2)
				error(0, Ebadarg);
			dkcall(Dial, c, field[1], 0, 0);
		} else if(strcmp(field[0], "announce")==0){
			if(m < 2)
				error(0, Ebadarg);
			dkcall(Announce, c, field[1], 0, 0);
		} else if(strcmp(field[0], "redial")==0){
			if(m < 4)
				error(0, Ebadarg);
			dkcall(Redial, c, field[1], field[2], field[3]);
		} else if(strcmp(field[0], "accept")==0){
			if(m < 2)
				error(0, Ebadarg);
			dkanswer(c, strtoul(field[1], 0, 0), 0);
		} else if(strcmp(field[0], "reject")==0){
			if(m < 3)
				error(0, Ebadarg);
			dkanswer(c, strtoul(field[1], 0, 0), strtoul(field[2], 0, 0));
		} else
			return streamwrite(c, a, n, 0);
		return n;
	}

	error(0, Eperm);
}

void	 
dkremove(Chan *c)
{
	error(0, Eperm);
}

void	 
dkwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void	 
dkerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void	 
dkuserstr(Error *e, char *buf)
{
	extern consuserstr(Error *, char *);

	consuserstr(e, buf);
}

/*
 *  send a message to the datakit on the common signaling line
 */
static int
dkmesg(Dk *dp, int type, int srv, int p0, int p1)
{
	Dkmsg d;
	Block *bp;

	if(dp->csc == 0)
		return -1;
	if(waserror())
		return -1;
	d.type = type;
	d.srv = srv;
	d.param0l = p0;
	d.param0h = p0>>8;
	d.param1l = p1;
	d.param1h = p1>>8;
	d.param2l = 0;
	d.param2h = 0;
	d.param3l = 0;
	d.param3h = 0;
	d.param4l = 0;
	d.param4h = 0;
	streamwrite(dp->csc, (char *)&d, sizeof(Dkmsg), 1);
	poperror();
	return 0;
}

/*
 *  call out on a datakit
 */
static int
calldone(void *a)
{
	Line *lp;

	lp = (Line *)a;
	return lp->state != Ldialing;
}
static void
dkcall(int type, Chan *c, char *addr, char *nuser, char *machine)
{
 	char dialstr[66];
	int line;
	char dialtone;
	int t_val, d_val;
	Dk *dp;
	Line *lp;
	Chan *dc;

	line = STREAMID(c->qid);
	dp = &dk[c->dev];
	lp = &dp->line[line];

	/*
	 *  only dial on virgin lines
	 */
	if(lp->state != Lclosed)
		error(0, Ebadarg);

	DPRINT("dkcall(line=%d, type=%d, dest=%s)\n", line, type, addr);

	/*
	 *  build dial string (guard against new lines)
	 */
	if(strchr(addr, '\n'))
		error(0, Ebadarg);
	if(strlen(addr)+strlen(u->p->pgrp->user)+2 >= sizeof(dialstr))
		error(0, Ebadarg);
	strcpy(dialstr, addr);
	switch(type){
	case Dial:
		t_val = T_SRV;
		d_val = D_DIAL;
		strcat(dialstr, "\n");
		strcat(dialstr, u->p->pgrp->user);
		strcat(dialstr, "\n");
		break;
	case Announce:
		t_val = T_SRV;
		d_val = D_SERV;
		break;
	case Redial:
		t_val = T_CHG;
		d_val = D_REDIAL;
		strcat(dialstr, "\n");
		strcat(dialstr, nuser);
		strcat(dialstr, "\n");
		strcat(dialstr, machine);
		strcat(dialstr, "\n");
		break;
	}

	/*
	 *  open the data file
	 */
	dc = dkattach(dp->name);
	if(waserror()){
		close(dc);
		nexterror();
	}
	dc->qid = STREAMQID(line, Sdataqid);
	dkopen(dc, ORDWR);

	lp->calltolive = 4;
	lp->state = Ldialing;

	/*
	 *  tell the controller we want to make a call
	 */
	DPRINT("dialout\n");
	dkmesg(dp, t_val, d_val, line, W_WINDOW(WS_2K,WS_2K,2));

	/*
	 *  if redial, wait for a dial tone (otherwise we might send
	 *  the dialstr to the previous other end and not the controller)
	 */
	if(type==Redial){
		if(streamread(dc, &dialtone, 1L) != 1L){
			lp->state = Lconnected;
			error(0, Ebadarg);
		}
	}

	/*
	 *  make the call
	 */
	DPRINT("dialstr %s\n", dialstr);
	streamwrite(dc, dialstr, (long)strlen(dialstr), 1);
	close(dc);
	poperror();

	/*
	 *  redial's never get a reply, assume it worked
	 */
	if(type == Redial) {
		lp->state = Lconnected;
		return;
	}

	/*
	 *  wait for a reply
	 */
	DPRINT("reply wait\n");
	sleep(&lp->r, calldone, lp);

	/*
	 *  if there was an error, translate it to a plan 9
	 *  errno and report it to the user.
	 */
	DPRINT("got reply %d\n", lp->state);
	if(lp->state != Lconnected) {
		if(lp->err >= DKERRS)
			error(0, dkerr[0]);
		else
			error(0, dkerr[lp->err]);
	}

	/*
	 *  linege state if serving
	 */
	if(type == D_SERV){
		lp->state = Llistening;
	}
	DPRINT("connected!\n");

	/*
	 *  decode the window size
	 */
	if (W_VALID(lp->window)){
		/*
		 *  a 1127 window negotiation
		 */
		lp->window = W_VALUE(W_DEST(lp->window));
	} else if(lp->window>2 && lp->window<31){
		/*
		 *  a generic window negotiation
		 */
		lp->window = 1<<lp->window;
	} else
		lp->window = 0;

	/*
	 *  tag the connection
	 */
	strncpy(lp->addr, addr, sizeof(lp->addr)-1);
	strncpy(lp->raddr, addr, sizeof(lp->raddr)-1);

	/*
	 *  reset the protocol
	 */
	dkwindow(c);
}

/*
 *  listen for a call, reflavor the 
 */
static int
dklisten(Chan *c)
{
	char dialstr[512];
	char *line[12];
	char *field[8];
	Line *lp;
	Dk *dp;
	int n, lineno, ts, window;
	Chan *dc;

	dp = &dk[c->dev];

	/*
	 *  open the data file
	 */
	dc = dkattach(dp->name);
	if(waserror()){
		close(dc);
		nexterror();
	}
	dc->qid = STREAMQID(STREAMID(c->qid), Sdataqid);
	dkopen(dc, ORDWR);

	/*
	 *  wait for a call in
	 */
	for(;;){
		/*
		 *  read the dialstring and null terminate it
		 */
		n = streamread(dc, dialstr, sizeof(dialstr)-1);
		DPRINT("returns %d\n", n);
		if(n <= 0)
			error(0, Eio);
		dialstr[n] = 0;
		DPRINT("dialstr = %s\n", dialstr);

		/*
		 *  break the dial string into lines
		 */
		n = getfields(dialstr, line, 12, '\n');
		if (n < 2) {
			DPRINT("bad dialstr from dk (1 line)\n");
			error(0, Eio);
		}

		/*
		 * line 0 is `line.tstamp.traffic[.urpparms.window]'
		 */
		window = 0;
		switch(getfields(line[0], field, 5, '.')){
		case 5:
			/*
			 *  generic way of passing window
			 */
			window = strtoul(field[4], 0, 0);
			if(window > 0 && window <31)
				window = 1<<window;
			else
				window = 0;
			/*
			 *  intentional fall through
			 */
		case 3:
			/*
			 *  1127 way of passing window
			 */
			if(window == 0){
				window = strtoul(field[2], 0, 0);
				if(W_VALID(window))
					window = W_VALUE(W_ORIG(window));
				else
					window = 0;
			}
			break;
		default:
			print("bad message from dk(bad first line)\n");
			continue;
		}
		lineno = strtoul(field[0], 0, 0);
		if(lineno >= dp->lines){
			print("dklisten: illegal line %d\n", lineno);
			continue;
		}
		lp = &dp->line[lineno];
		ts = strtoul(field[1], 0, 0);

		/*
		 *  this could be a duplicate request
		 */
		if(ts == lp->timestamp){
			print("dklisten: repeat timestamp %d\n", lineno);
			continue;
		}
	
		/*
		 *  take care of glare (datakit picked an inuse channel
		 *  for the call to come in on).
		 */
		if(!canqlock(lp)){
			DPRINT("DKbusy1\n");
			dkanswer(c, lineno, DKbusy);
			continue;
		} else {
			if(lp->state != Lclosed){
				qunlock(lp);
				DPRINT("DKbusy2 %ux\n", lp->state);
				dkanswer(c, lineno, DKbusy);
				continue;
			}
		}
		lp->window = window;

		/*
		 *  Line 1 is `my-dk-name.service[.more-things]'.
		 *  Special characters are escaped by '\'s.
		 */
		strncpy(lp->addr, line[1], sizeof(lp->addr)-1);
	
		/*
		 *  the rest is variable length
		 */
		switch(n) {
		case 2:
			/* no more lines */
			lp->ruser[0] = 0;
			lp->raddr[0] = 0;
			break;
		case 3:
			/* line 2 is `source.user.param1.param2' */
			getfields(line[2], field, 3, '.');
			strncpy(lp->raddr, field[0], sizeof(lp->raddr)-1);
			strncpy(lp->ruser, field[1], sizeof(lp->ruser)-1);
			break;
		case 4:
			/* line 2 is `user.param1.param2' */
			getfields(line[2], field, 2, '.');
			strncpy(lp->ruser, field[0], sizeof(lp->ruser)-1);
	
			/* line 3 is `source.node.mod.line' */
			strncpy(lp->raddr, line[3], sizeof(lp->raddr)-1);
			break;
		default:
			print("bad message from dk(>4 line)\n");
			qunlock(lp);
			error(0, Ebadarg);
		}

		sprint(lp->other, "w(%d)", W_TRAF(lp->window));
		DPRINT("src(%s)user(%s)dest(%s)other(%s)\n", lp->raddr, lp->ruser,
			lp->addr, lp->other);

		lp->timestamp = ts;
		lp->state = Lconnected;
		qunlock(lp);
		close(dc);
		poperror();
		DPRINT("dklisten returns %d\n", lineno);
		return lineno;
	}
}

/*
 *  answer a call
 */
static void
dkanswer(Chan *c, int line, int code)
{
	char reply[64];
	Dk *dp;
	Chan *dc;
	Line *lp;

	dp = &dk[c->dev];
	lp = &dp->line[line];

	/*
	 *  open the data file (c is a control file)
	 */
	dc = dkattach(dp->name);
	if(waserror()){
		close(dc);
		nexterror();
	}
	dc->qid = STREAMQID(STREAMID(c->qid), Sdataqid);
	dkopen(dc, ORDWR);

	/*
	 *  send the reply
	 */
	sprint(reply, "%ud.%ud.%ud", line, lp->timestamp, code);
	DPRINT("dkanswer %s\n", reply);
	streamwrite(dc, reply, strlen(reply), 1);
	close(dc);
	poperror();
}

/*
 *  set the window size and reset the protocol
 */
static void
dkwindow(Chan *c)
{
	char buf[64];
	long wins;
	Line *lp;

	lp = &dk[c->dev].line[STREAMID(c->qid)];
	if(lp->window == 0)
		lp->window = 64;
	sprint(buf, "init %d %d", lp->window, Streamhi);
	streamwrite(c, buf, strlen(buf), 1);
}

/*
 *  hangup a datakit connection
 */
static void
dkhangup(Line *lp)
{
	Block *bp;

	qlock(lp);
	if(lp->rq){
		bp = allocb(0);
		bp->type = M_HANGUP;
		PUTNEXT(lp->rq, bp);
	}
	qunlock(lp);
}

/*
 *  A process which listens to all input on a csc line
 */
static void
dkcsckproc(void *a)
{
	long n;
	Dk *dp;
	Dkmsg d;
	int line;
	int i;

	dp = (Dk *)a;

	/*
	 *  loop forever listening
	 */
	for(;;){
		n = streamread(dp->csc, (char *)&d, (long)sizeof(d));
		if(n != sizeof(d)){
			print("strange csc message %d\n", n);
			continue;
		}
		line = (d.param0h<<8) + d.param0l;
/*		print("t(%d)s(%d)l(%d)\n", d.type, d.srv, line); /**/
		switch (d.type) {

		case T_CHG:	/* controller wants to close a line */
			dkchgmesg(dp, &d, line);
			break;
		
		case T_REPLY:	/* reply to a dial request */
			dkreplymesg(dp, &d, line);
			break;
		
		case T_SRV:	/* ignore it, it's useless */
			print("dksrvmesg(%d)\n", line);
			break;
		
		case T_RESTART:	/* datakit reboot */
			print("dk restart\n");
			if(line >=0 && line<dp->lines){
				print("maxlines=%d\n", line+1);
				dp->lines=line+1;
			}
			break;
		
		default:
			DPRINT("unrecognized csc message %o(%o)\n", d.type, line);
			break;
		}
	}
}

/*
 *  datakit requests or confirms closing a line
 */
static void
dkchgmesg(Dk *dp, Dkmsg *dialp, int line)
{
	Line *lp;

	if (line <= 0 || line >= dp->lines) {
		/* tell controller this line is not in use */
		dkmesg(dp, T_CHG, D_CLOSE, line, 0);
		return;
	}
	lp = &dp->line[line];
	switch (dialp->srv) {

	case D_CLOSE:		/* remote shutdown */
		switch (lp->state) {

		case Ldialing:
			/* simulate a failed connection */
			dkreplymesg(dp, (Dkmsg *)0, line);
			lp->state = Lrclose;
			break;

		case Lrclose:
		case Lconnected:
		case Llistening:
		case Lackwait:
			dkhangup(lp);
			lp->state = Lrclose;
			break;

		case Lopened:
			dkmesg(dp, T_CHG, D_CLOSE, line, 0);
			break;

		case Llclose:
		case Lclosed:
			dkhangup(lp);
			dkmesg(dp, T_CHG, D_CLOSE, line, 0);
			lp->state = Lclosed;
			break;
		}
		break;
	
	case D_ISCLOSED:	/* acknowledging a local shutdown */
		switch (lp->state) {
		case Llclose:
		case Lclosed:
			lp->state = Lclosed;
			break;

		case Lrclose:
		case Lconnected:
		case Llistening:
		case Lackwait:
			break;
		}
		break;

	default:
		print("unrecognized T_CHG\n");
	}
}

/*
 *  datakit replies to a dialout.  capture reply code and traffic parameters
 */
static void
dkreplymesg(Dk *dp, Dkmsg *dialp, int line)
{
	Proc *p;
	Line *lp;

	DPRINT("dkreplymesg(%d)\n", line);

	if(line < 0 || line >= dp->lines)
		return;

	lp=&dp->line[line];
	if(lp->state != Ldialing)
		return;

	if(dialp){
		/*
		 *  a reply from the dk
		 */
		lp->state = (dialp->srv==D_OPEN) ? Lconnected : Lrclose;
		lp->err = (dialp->param1h<<8) + dialp->param1l;
		lp->window = lp->err;
		DPRINT("dkreplymesg: %d\n", lp->state);
	} else {
		/*
		 *  a local abort
		 */
		lp->state = Lrclose;
		lp->err = 0;
	}

	if(lp->state==Lrclose){
		dkhangup(lp);
	}
	wakeup(&lp->r);
}

/*
 *  15-second timer for all interfaces
 */
static Rendez dkt;
static int
fuckit(void *a)
{
	return 0;
}
static void
dktimer(void *a)
{
	int dki, i;
	Dk *dp;
	Line *lp;

	waserror();

	for(;;){
		/*
		 *  loop through the active dks
		 */
		for(dki=0; dki<Ndk; dki++){
			dp = &dk[dki];
			if(dp->csc==0)
				continue;

			/*
			 * send keep alive
			 */
			dkmesg(dp, T_ALIVE, D_CONTINUE, 0, 0);

			/*
			 *  remind controller of dead lines and
			 *  timeout calls that take to long
			 */
			for (i=0; i<dp->lines; i++){
				lp = &dp->line[i];
				switch(lp->state){
				case Llclose:
					dkmesg(dp, T_CHG, D_CLOSE, i, 0);
					break;

				case Ldialing:
					if(lp->calltolive==0 || --lp->calltolive!=0)
						break;
					dkreplymesg(dp, (Dkmsg *)0, i);
					break;
				}
			}
		}
		tsleep(&dkt, fuckit, 0, 7500);
	}
}
