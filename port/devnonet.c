#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#define DPRINT if(pnonet)print
#define NOW (MACHP(0)->ticks*MS2HZ)

static Noifc *noifc;
int pnonet;

enum {
	/*
	 *  tuning parameters
	 */
	MSrexmit = 250,		/* retranmission interval in ms */
	MSack = 50,		/* ms to sit on an ack */

	/*
	 *  relative or immutable
	 */
	Nsubdir = 5,		/* entries in the nonet directory */
	Nmask = Nnomsg - 1,	/* mask for log(Nnomsg) bits */
};

/* predeclared */
static void	hangup(Noconv*);
static Block*	mkhdr(Noconv*, int);
static void	listen(Chan*, Noifc*);
static void	announce(Chan*, char*);
static void	connect(Chan*, char*);
static void	rcvack(Noconv*, int);
static void	sendctlmsg(Noconv*, int, int);
static void	sendmsg(Noconv*, Nomsg*);
static void	startconv(Noconv*, int, char*, int);
static void	queueack(Noconv*, int);
static void	nonetkproc(void*);
static void	nonetiput(Queue*, Block*);
static void	nonetoput(Queue*, Block*);
static void	nonetstclose(Queue*);
static void	nonetstopen(Queue*, Stream*);

extern Qinfo	noetherinfo;
extern Qinfo	nonetinfo;

/*
 *  nonet directory and subdirectory
 */
enum {
	/*
	 *  per conversation
	 */
	Naddrqid,
	Nlistenqid,
	Nraddrqid,
	Nruserqid,
	Nstatsqid,
	Nchanqid,

	/*
	 *  per noifc
	 */
	Ncloneqid,
};

Dirtab *nonetdir;
Dirtab nosubdir[]={
	"addr",		{Naddrqid},	0,	0600,
	"listen",	{Nlistenqid},	0,	0600,
	"raddr",	{Nraddrqid},	0,	0600,
	"ruser",	{Nruserqid},	0,	0600,
	"stats",	{Nstatsqid},	0,	0600,
};

/*
 *  Nonet conversation states (for Noconv.state)
 */
enum {
	Cclosed,
	Copen,
	Cannounced,
	Cconnected,
	Cconnecting,
	Chungup,
	Cclosing,
};

/*
 *  nonet kproc
 */
static int kstarted;
static Rendez nonetkr;

/*
 *  nonet file system.  most of the calls use dev.c to access the nonet
 *  directory and stream.c to access the nonet devices.
 *  create the nonet directory.  the files are `clone' and stream
 *  directories '1' to '32' (or whatever conf.noconv is in decimal)
 */
void
nonetreset(void)
{
	int i;

	/*
	 *  allocate the interfaces
	 */
	noifc = (Noifc *)ialloc(sizeof(Noifc) * conf.nnoifc, 0);
	for(i = 0; i < conf.nnoifc; i++)
		noifc[i].conv = (Noconv *)ialloc(sizeof(Noconv) * conf.nnoconv, 0);

	/*
	 *  create the directory.
	 */
	nonetdir = (Dirtab *)ialloc(sizeof(Dirtab) * (conf.nnoconv+1), 0);

	/*
	 *  the circuits
	 */
	for(i = 0; i < conf.nnoconv; i++) {
		sprint(nonetdir[i].name, "%d", i);
		nonetdir[i].qid.path = CHDIR|STREAMQID(i, Nchanqid);
		nonetdir[i].qid.vers = 0;
		nonetdir[i].length = 0;
		nonetdir[i].perm = 0600;
	}

	/*
	 *  the clone device
	 */
	strcpy(nonetdir[i].name, "clone");
	nonetdir[i].qid.path = Ncloneqid;
	nonetdir[i].qid.vers = 0;
	nonetdir[i].length = 0;
	nonetdir[i].perm = 0600;
}

void
nonetinit(void)
{
}

/*
 *  find an noifc and increment its count
 */
Chan*
nonetattach(char *spec)
{
	Noifc *ifc;
	Chan *c;

	/*
	 *  find an noifc with the same name
	 */
	if(*spec == 0)
		spec = "nonet";
	for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
		lock(ifc);
		if(strcmp(spec, ifc->name)==0 && ifc->wq) {
			ifc->ref++;
			unlock(ifc);
			break;
		}
		unlock(ifc);
	}
	if(ifc == &noifc[conf.nnoifc])
		error(Enoifc);
	c = devattach('n', spec);
	c->dev = ifc - noifc;

	return c;
}

/*
 *  up the reference count to the noifc
 */
Chan*
nonetclone(Chan *c, Chan *nc)
{
	Noifc *ifc;

	c = devclone(c, nc);
	ifc = &noifc[c->dev];
	lock(ifc);
	ifc->ref++;
	unlock(ifc);
	return c;
}

int	 
nonetwalk(Chan *c, char *name)
{
	if(c->qid.path == CHDIR)
		return devwalk(c, name, nonetdir, conf.nnoconv+1, devgen);
	else
		return devwalk(c, name, nosubdir, Nsubdir, streamgen);
}

void	 
nonetstat(Chan *c, char *dp)
{
	if(c->qid.path == CHDIR)
		devstat(c, dp, nonetdir, conf.nnoconv+1, devgen);
	else if(c->qid.path == Ncloneqid)
		devstat(c, dp, nonetdir, 1, devgen);
	else
		devstat(c, dp, nosubdir, Nsubdir, streamgen);
}

/*
 *  opening a nonet device allocates a Noconv.  Opening the `clone'
 *  device is a ``macro'' for finding a free Noconv and opening
 *  it's ctl file.
 */
Noconv *
nonetopenclone(Chan *c, Noifc *ifc)
{
	Noconv *cp;
	Noconv *ep;

	ep = &ifc->conv[conf.nnoconv];
	for(cp = &ifc->conv[0]; cp < ep; cp++){
		if(cp->state == Cclosed && canqlock(cp)){
			if(cp->state != Cclosed){
				qunlock(cp);
				continue;
			}
			c->qid.path = CHDIR|STREAMQID(cp-ifc->conv, Nchanqid);
			devwalk(c, "ctl", 0, 0, streamgen);
			streamopen(c, &nonetinfo);
			qunlock(cp);
			break;
		}
	}
	if(cp == ep)
		error(Enodev);;
	return cp;
}

Chan*
nonetopen(Chan *c, int omode)
{
	Stream *s;
	Noifc *ifc;
	int line;

	if(!kstarted){
		kproc("nonetack", nonetkproc, 0);
		kstarted = 1;
	}

	if(c->qid.path & CHDIR){
		/*
		 *  directories are read only
		 */
		if(omode != OREAD)
			error(Ebadarg);
	} else switch(STREAMTYPE(c->qid.path)){
	case Ncloneqid:
		/*
		 *  get an unused device and open it's control file
		 */
		ifc = &noifc[c->dev];
		nonetopenclone(c, ifc);
		break;
	case Nlistenqid:
		/*
		 *  listen for a call and open the control file for the
		 *  channel on which the call arrived.
		 */
		line = STREAMID(c->qid.path);
		ifc = &noifc[c->dev];
		if(ifc->conv[line].state != Cannounced)
			error(Enoannounce);
		listen(c, ifc);
		break;
	case Nraddrqid:
		/*
		 *  read only files
		 */
		if(omode != OREAD)
			error(Ebadarg);
		break;
	default:
		/*
		 *  open whatever c points to
		 */
		streamopen(c, &nonetinfo);
		break;
	}

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
nonetcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
nonetclose(Chan *c)
{
	Noifc *ifc;

	/*
	 *  real closing happens in nonetstclose
	 */
	if(c->qid.path != CHDIR)
		streamclose(c);

	/*
	 *  let go of the multiplexor
	 */
	nonetfreeifc(&noifc[c->dev]);
}

long	 
nonetread(Chan *c, void *a, long n)
{
	int t;
	Noconv *cp;
	char stats[256];

	/*
	 *  pass data/ctl reads onto the stream code
	 */
	t = STREAMTYPE(c->qid.path);
	if(t >= Slowqid)
		return streamread(c, a, n);

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, nonetdir, conf.nnoconv+1, devgen);
	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, nosubdir, Nsubdir, streamgen);

	cp = &noifc[c->dev].conv[STREAMID(c->qid.path)];
	switch(t){
	case Nraddrqid:
		return stringread(c, a, n, cp->raddr);
	case Naddrqid:
		return stringread(c, a, n, cp->addr);
	case Nruserqid:
		return stringread(c, a, n, cp->ruser);
	case Nstatsqid:
		sprint(stats, "sent: %d\nrcved: %d\nrexmit: %d\nbad: %d\n",
			cp->sent, cp->rcvd, cp->rexmit, cp->bad);
		return stringread(c, a, n, stats);
	}
	error(Eperm);
}

long	 
nonetwrite(Chan *c, void *a, long n)
{
	int t;
	int m;
	char buf[256];
	char *field[5];

	t = STREAMTYPE(c->qid.path);

	/*
	 *  get data dispatched as quickly as possible
	 */
	if(t == Sdataqid)
		return streamwrite(c, a, n, 0);

	/*
	 *  easier to do here than in nonetoput
	 */
	if(t == Sctlqid){
		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 5, ' ');
		if(strcmp(field[0], "connect")==0){
			if(m < 2)
				error(Ebadarg);
			connect(c, field[1]);
		} else if(strcmp(field[0], "announce")==0){
			announce(c, field[1]);
		} else if(strcmp(field[0], "accept")==0){
			/* ignore */;
		} else if(strcmp(field[0], "reject")==0){
			/* ignore */;
		} else
			return streamwrite(c, a, n, 0);
		return n;
	}
	
	error(Eperm);
}

void	 
nonetremove(Chan *c)
{
	error(Eperm);
}

void	 
nonetwstat(Chan *c, char *dp)
{
	error(Eperm);
}

/*
 *  the device stream module definition
 */
Qinfo nonetinfo =
{
	nonetiput,
	nonetoput,
	nonetstopen,
	nonetstclose,
	"nonet"
};

/*
 *  store the device end of the stream so that the multiplexor can
 *  send blocks upstream.
 */
static void
nonetstopen(Queue *q, Stream *s)
{
	Noifc *ifc;
	Noconv *cp;

	ifc = &noifc[s->dev];
	cp = &ifc->conv[s->id];
	cp->s = s;
	cp->ifc = ifc;
	cp->rq = RD(q);
	cp->state = Copen;
	RD(q)->ptr = WR(q)->ptr = (void *)cp;
}

/*
 *  wait until a hangup is received.
 *  then send a hangup message (until one is received).
 */
static int
ishungup(void *a)
{
	Noconv *cp;

	cp = (Noconv *)a;
	return cp->state == Chungup;
}
static void
nonetstclose(Queue *q)
{
	Noconv *cp;
	Nomsg *mp;
	int i;

	cp = (Noconv *)q->ptr;

	if(waserror()){
		cp->rcvcircuit = -1;
		cp->state = Cclosed;
		nexterror();
	}

	/*
	 *  send hangup messages to the other side
	 *  until it hangs up or we get tired.
	 */
	if(cp->state >= Cconnected){
		sendctlmsg(cp, NO_HANGUP, 1);
		for(i=0; i<10 && !ishungup(cp); i++){
			sendctlmsg(cp, NO_HANGUP, 1);
			tsleep(&cp->r, ishungup, cp, MSrexmit);
		}
	}

	qlock(cp);
	cp->rcvcircuit = -1;
	cp->state = Cclosed;
	qunlock(cp);
	poperror();
}

/*
 *  send all messages up stream.  this should only be control messages
 */
static void
nonetiput(Queue *q, Block *bp)
{
	Noconv *cp;

	if(bp->type == M_HANGUP){
		cp = (Noconv *)q->ptr;
		cp->state = Chungup;
	}
	PUTNEXT(q, bp);
}

/*
 *  queue a block
 */
static int
windowopen(void *a)
{
	Noconv *cp;

	cp = (Noconv *)a;
	return cp->out[cp->next].inuse == 0;	
}
static int
acked(void *a)
{
	Nomsg *mp;

	mp = (Nomsg *)a;
	return mp->inuse;
}
static void
nonetoput(Queue *q, Block *bp)
{
	Noconv *cp;
	int next;
	Nomsg *mp;
	int retries;

	cp = (Noconv *)(q->ptr);

	/*
	 *  do all control functions
	 */
	if(bp->type != M_DATA){
		freeb(bp);
		error(Ebadctl);
	}

	/*
	 *  collect till we see a delim
	 */
	qlock(&cp->mlock);
	if(!putb(q, bp)){
		qunlock(&cp->mlock);
		return;
	}

	mp = 0;
	if(waserror()){
		if(mp){
			q->len = 0;
			q->first = q->last = 0;
			if(mp->first){
				freeb(mp->first);
				mp->first = 0;
			}
			mp->inuse = 0;
			mp->acked = 0;
			if(((cp->first+1)%Nnomsg) == cp->next)
				cp->first = cp->next;
		}
		qunlock(&cp->mlock);
		nexterror();
	}

	/*
	 *  block till we get a buffer
	 */
	while(cp->out[cp->next].inuse)
		sleep(&cp->r, windowopen, cp);
	mp = &cp->out[cp->next];
	mp->inuse = 1;
	cp->next = (cp->next+1)%Nnomsg;

	/*
	 *  stick the message in a Nomsg structure
	 */
	mp->time = NOW + MSrexmit;
	mp->first = q->first;
	mp->last = q->last;
	mp->len = q->len;
	mp->mid ^= Nnomsg;
	mp->acked = 0;

	/*
	 *  init the queue for new messages
	 */
	q->len = 0;
	q->first = q->last = 0;
	cp->sent++;

	/*
	 *  send the message, the kproc will retry
	 */
	sendmsg(cp, mp);
	qunlock(&cp->mlock);
	poperror();
}

/*
 *  start a new conversation.  start an ack/retransmit process if
 *  none already exists for this circuit.
 */
void
startconv(Noconv *cp, int circuit, char *raddr, int state)
{
	int i;
	char name[32];
	Noifc *ifc;

	ifc = cp->ifc;

	/*
	 *  allocate the prototype header
	 */
	cp->media = allocb(ifc->hsize + NO_HDRSIZE);
	cp->media->wptr += ifc->hsize + NO_HDRSIZE;
	cp->hdr = (Nohdr *)(cp->media->rptr + ifc->hsize);

	/*
	 *  fill in the circuit number
	 */
	cp->hdr->flag = NO_NEWCALL|NO_ACKME;
	cp->hdr->circuit[2] = circuit>>16;
	cp->hdr->circuit[1] = circuit>>8;
	cp->hdr->circuit[0] = circuit;

	/*
	 *  set the state variables
	 */
	cp->state = state;
	for(i = 1; i < Nnomsg; i++){
		cp->in[i].mid = i;
		cp->in[i].acked = 0;
		cp->in[i].rem = 0;
		cp->out[i].mid = i | Nnomsg;
		cp->out[i].acked = 1;
		cp->out[i].rem = 0;
		cp->out[i].inuse = 0;
	}
	cp->in[0].mid = Nnomsg;
	cp->in[0].acked = 0;
	cp->in[0].rem = 0;
	cp->out[0].mid = 0;
	cp->out[0].acked = 1;
	cp->out[0].rem = 0;
	cp->first = cp->next = 1;
	cp->rexmit = cp->bad = cp->sent = cp->rcvd = cp->lastacked = 0;

	/*
	 *  used for demultiplexing
	 */
	cp->rcvcircuit = circuit ^ 1;

	/*
	 *  media dependent header
	 */
	(*ifc->connect)(cp, raddr);

	/*
	 *  status files
	 */
	strncpy(cp->raddr, raddr, sizeof(cp->raddr));
	strcpy(cp->ruser, "none");
}

/*
 *  announce willingness to take calls
 */
static void
announce(Chan *c, char *addr)
{
	Noconv *cp;

	cp = (Noconv *)(c->stream->devq->ptr);
	if(cp->state != Copen)
		error(Einuse);
	cp->state = Cannounced;
}

/*
 *  connect to the destination whose name is pointed to by bp->rptr.
 *
 *  a service is separated from the destination system by a '!'
 */
static void
connect(Chan *c, char *addr)
{
	Noifc *ifc;
	Noconv *cp;
	char *service;
	char buf[2*NAMELEN+2];

	cp = (Noconv *)(c->stream->devq->ptr);
	if(cp->state != Copen)
		error(Einuse);
	ifc = cp->ifc;
	service = strchr(addr, '!');
	if(service){
		*service++ = 0;
		if(strchr(service, ' '))
			error(Ebadctl);
		if(strlen(service) > NAMELEN)
			error(Ebadctl);
	}

	startconv(cp, 2*(cp - ifc->conv), addr, Cconnecting);

	if(service){
		/*
		 *  send service name and user name
		 */
		cp->hdr->flag |= NO_SERVICE;
		sprint(buf, "%s %s", service, u->p->pgrp->user);
		c->qid.path = STREAMQID(STREAMID(c->qid.path), Sdataqid);
		DPRINT("sending request\n");
		streamwrite(c, buf, strlen(buf), 1);
		DPRINT("request sent\n");
		c->qid.path = STREAMQID(STREAMID(c->qid.path), Sctlqid);
	}
}

/*
 *  listen for a call.  There can be many listeners, but only one can sleep
 *  on the circular queue at a time.  ifc->listenl lets only one at a time into
 *  the sleep.
 */
static int
iscall(void *a)
{
	Noifc *ifc;

	ifc = (Noifc *)a;
	return ifc->rptr != ifc->wptr;
}
static void
listen(Chan *c, Noifc *ifc)
{
	Noconv *cp, *ep;
	Nocall call;
	int f;
	char buf[2*NAMELEN+4];
	char *user;
	long n;

	call.msg = 0;

	if(waserror()){
		if(call.msg)
			freeb(call.msg);
		qunlock(&ifc->listenl);
		nexterror();
	}

	for(;;){
		/*
		 *  one listener at a time
		 */
		qlock(&ifc->listenl);

		/*
		 *  wait for a call
		 */
		sleep(&ifc->listenr, iscall, ifc);
		call = ifc->call[ifc->rptr];
		ifc->rptr = (ifc->rptr+1) % Nnocalls;
		qunlock(&ifc->listenl);

		/*
		 *  make sure we aren't already using this circuit
		 */
		ep = &ifc->conv[conf.nnoconv];
		for(cp = &ifc->conv[0]; cp < ep; cp++){
			if(cp->state>Cannounced && (call.circuit^1)==cp->rcvcircuit
			&& strcmp(call.raddr, cp->raddr)==0)
				break;
		}
		if(cp != ep){
			freeb(call.msg);
			call.msg = 0;
			continue;
		}

		/*
		 *  get a free channel
		 */
		cp = nonetopenclone(c, ifc);
		c->qid.path = STREAMQID(cp - ifc->conv, Sctlqid);

		/*
		 *  start the protocol and
		 *  stuff the connect message into it
		 */
		f = ((Nohdr *)(call.msg->rptr))->flag;
		DPRINT("call from %d %s\n", call.circuit, call.raddr);
		startconv(cp, call.circuit, call.raddr, Cconnecting);
		DPRINT("rcving %d byte message\n", call.msg->wptr - call.msg->rptr);
		nonetrcvmsg(cp, call.msg);
		call.msg = 0;

		/*
		 *  if a service and remote user were specified,
		 *  grab them
		 */
		if(f & NO_SERVICE){
			DPRINT("reading service\n");
			c->qid.path = STREAMQID(cp - ifc->conv, Sdataqid);
			n = streamread(c, buf, sizeof(buf));
			c->qid.path = STREAMQID(cp - ifc->conv, Sctlqid);
			if(n <= 0)
				error(Ebadctl);
			buf[n] = 0;
			user = strchr(buf, ' ');
			if(user){
				*user++ = 0;
				strncpy(cp->ruser, user, NAMELEN);
			} else
				strcpy(cp->ruser, "none");
			strncpy(cp->addr, buf, NAMELEN);
		}
		break;
	}
	poperror();
}

/*
 *  send a hangup signal up the stream to get all line disciplines
 *  to cease and desist
 */
static void
hangup(Noconv *cp)
{
	Block *bp;
	Queue *q;

	cp->state = Chungup;
	bp = allocb(0);
	bp->type = M_HANGUP;
	q = cp->rq;
	PUTNEXT(q, bp);
	wakeup(&cp->r);
}

/*
 *  process a message acknowledgement.  if the message
 *  has any xmit buffers queued, free them.
 */
static void
rcvack(Noconv *cp, int mid)
{
	Nomsg *mp;
	Block *bp;
	int i;

	mp = &cp->out[mid & Nmask];

	/*
	 *  if already acked, ignore
	 */
	if(mp->acked || mp->mid != mid)
		return;

	/*
 	 *  free it
	 */
	cp->rexmit = 0;
	mp->acked = 1;
	cp->lastacked = mid;
	freeb(mp->first);
	mp->first = 0;
	mp->inuse = 0;

	/*
	 *  advance first if this is the first
	 */
	if((mid&Nmask) == cp->first){
		while(cp->first != cp->next){
			if(cp->out[cp->first].acked == 0)
				break;
			cp->first = (cp->first+1) % Nnomsg;
		}
	}
}

/*
 *  queue an acknowledgement to be sent.  ignore it if we already have Nnomsg
 *  acknowledgements queued.
 */
static void
queueack(Noconv *cp, int mid)
{
	int next;

	next = (cp->anext + 1)&Nmask;
	if(next != cp->afirst){
		cp->ack[cp->anext] = mid;
		cp->anext = next;
	}
}

/*
 *  make a packet header
 */
Block *
mkhdr(Noconv *cp, int rem)
{
	Block *bp;
	Nohdr *hp;

	bp = allocb(cp->ifc->hsize + NO_HDRSIZE + cp->ifc->mintu);
	memcpy(bp->wptr, cp->media->rptr, cp->ifc->hsize + NO_HDRSIZE);
	bp->wptr += cp->ifc->hsize + NO_HDRSIZE;
	hp = (Nohdr *)(bp->rptr + cp->ifc->hsize);
	hp->remain[1] = rem>>8;
	hp->remain[0] = rem;
	hp->sum[0] = hp->sum[1] = 0;
	return bp;
}

/*
 *  transmit a message.  this involves breaking a possibly multi-block message into
 *  a train of packets on the media.
 *
 *  called by nonetoput().  the qlock(mp) synchronizes these two
 *  processes.
 */
static void
sendmsg(Noconv *cp, Nomsg *mp)
{
	Noifc *ifc;
	Queue *wq;
	int msgrem;
	int pktrem;
	int n;
	Block *bp, *pkt, *last;
	uchar *rptr;

	ifc = cp->ifc;
	if(ifc == 0)
		return;
	wq = ifc->wq->next;

	/*
	 *  one transmitter at a time for this connection
	 */
	qlock(&cp->xlock);

	if(waserror()){
		qunlock(&cp->xlock);
		return;
	}

	/*
	 *  get the next acknowledge to use if the next queue up
	 *  is not full.
	 */
	if(cp->afirst != cp->anext && cp->rq->next->len < 16*1024){
		cp->hdr->ack = cp->ack[cp->afirst];
		cp->afirst = (cp->afirst+1)&Nmask;
	}
	cp->hdr->mid = mp->mid;

	if(ifc->mintu > mp->len) {
		/*
		 *  short message:
		 *  copy the whole message into the header block
		 */
		last = pkt = mkhdr(cp, mp->len);
		for(bp = mp->first; bp; bp = bp->next){
			memcpy(pkt->wptr, bp->rptr, n = BLEN(bp));
			pkt->wptr += n;
		}
		memset(pkt->wptr, 0, n = ifc->mintu - mp->len);
		pkt->wptr += n;
	} else {
		/*
		 *  long message:
		 *  break up the message into noifc packets and send them.
		 *  once around this loop for each non-header block generated.
		 */
		msgrem = mp->len;
		pktrem = msgrem > ifc->maxtu ? ifc->maxtu : msgrem;
		bp = mp->first;
		SET(rptr);
		if(bp)
			rptr = bp->rptr;
		last = pkt = mkhdr(cp, msgrem);
		while(bp){
			/*
			 *  if pkt full, send and create new header block
			 */
			if(pktrem == 0){
				nonetcksum(pkt, ifc->hsize);
				last->flags |= S_DELIM;
				(*wq->put)(wq, pkt);
				last = pkt = mkhdr(cp, -msgrem);
				pktrem = msgrem > ifc->maxtu ? ifc->maxtu : msgrem;
			}
			n = bp->wptr - rptr;
			if(n > pktrem)
				n = pktrem;
			last = last->next = allocb(0);
			last->rptr = rptr;
			last->wptr = rptr = rptr + n;
			msgrem -= n;
			pktrem -= n;
			if(rptr >= bp->wptr){
				bp = bp->next;
				if(bp)
					rptr = bp->rptr;
			}
		}
	}
	nonetcksum(pkt, ifc->hsize);
	last->flags |= S_DELIM;
	(*wq->put)(wq, pkt);
	mp->time = NOW + MSrexmit;
	qunlock(&cp->xlock);
	poperror();
}

/*
 *  send a control message (hangup or acknowledgement).
 */
static void
sendctlmsg(Noconv *cp, int flag, int new)
{
	Nomsg ctl;

	ctl.len = 0;
	ctl.first = 0;
	ctl.acked = 0;
	if(new)
		ctl.mid = Nnomsg^cp->out[cp->next].mid;
	else
		ctl.mid = cp->lastacked;
	cp->hdr->flag |= flag;
	sendmsg(cp, &ctl);
}

/*
 *  receive a message (called by the multiplexor; noetheriput, nofddiiput, ...)
 */
void
nonetrcvmsg(Noconv *cp, Block *bp)
{
	Block *nbp;
	Nohdr *h;
	short r;
	int c;
	Nomsg *mp;
	int f;
	Queue *q;

	q = cp->rq;

	/*
	 *  grab the packet header, push the pointer past the nonet header
	 */
	h = (Nohdr *)bp->rptr;
	bp->rptr += NO_HDRSIZE;
	mp = &cp->in[h->mid & Nmask];
	r = (h->remain[1]<<8) | h->remain[0];
	f = h->flag;

	/*
	 *  if a new call request comes in on a connected channel, hang up the call
	 */
	if(h->mid==0 && (f & NO_NEWCALL) && cp->state==Cconnected){
		DPRINT("new call on connected channel\n"); 
		freeb(bp);
		hangup(cp);
		return;
	}

	/*
	 *  ignore old messages and process the acknowledgement
	 */
	if(h->mid != mp->mid){
		DPRINT("old msg %d instead of %d r==%d\n", h->mid, mp->mid, r);
		if(r == 0){
			rcvack(cp, h->ack);
			if(f & NO_HANGUP)
				hangup(cp);
		} else {
			if(r>0){
				rcvack(cp, h->ack);
				queueack(cp, h->mid);
			}
			cp->bad++;
		}
		freeb(bp);
		return;
	}

	if(r>=0){
		/*
		 *  start of message packet
		 */
		if(mp->first){
			DPRINT("mp->mid==%d mp->rem==%d r==%d\n", mp->mid, mp->rem, r);
			freeb(mp->first);
			mp->first = mp->last = 0;
			mp->len = 0;
		}
		mp->rem = r;
	} else {
		/*
		 *  a continuation
		 */
		if(-r != mp->rem) {
			DPRINT("mp->mid==%d mp->rem==%d r==%d\n", mp->mid, mp->rem, r);
			cp->bad++;
			freeb(bp);
			return;
		}
	}

	/*
	 *  take care of packets that were padded up
	 */
	mp->rem -= BLEN(bp);
	if(mp->rem < 0){
		if(-mp->rem <= BLEN(bp)){
			bp->wptr += mp->rem;
			mp->rem = 0;
		} else
			panic("nonetrcvmsg: short packet");
	}
	putb(mp, bp);

	/*
	 *  if the last chunk - pass it up the stream and wake any
	 *  waiting process.
	 *
	 *  if not, strip off the delimiter.
	 */
	if(mp->rem == 0){
		rcvack(cp, h->ack);
		if(f & NO_ACKME)
			queueack(cp, h->mid);
		mp->last->flags |= S_DELIM;
		PUTNEXT(q, mp->first);
		mp->first = mp->last = 0;
		mp->len = 0;
		cp->rcvd++;

		/*
		 *  cycle bufffer to next expected mid
		 */
		mp->mid ^= Nnomsg;

		/*
		 *  stop xmitting the NO_NEWCALL flag
		 */
		if(cp->state==Cconnecting && !(f & NO_NEWCALL))
			cp->state = Cconnected;
	} else
		mp->last->flags &= ~S_DELIM;

}

/*
 *  noifc
 */
/*
 *  Create an ifc.
 */
Noifc *
nonetnewifc(Queue *q, Stream *s, int maxtu, int mintu, int hsize,
	void (*connect)(Noconv *, char *))
{
	Noifc *ifc;
	int i;

	for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
		if(ifc->ref == 0){
			lock(ifc);
			if(ifc->ref) {
				/* someone was faster than us */
				unlock(ifc);
				continue;
			}
			RD(q)->ptr = WR(q)->ptr = (void *)ifc;
			for(i = 0; i < conf.nnoconv; i++)
				ifc->conv[i].rcvcircuit = -1;
			ifc->maxtu = maxtu - hsize - NO_HDRSIZE;
			ifc->mintu = mintu - hsize - NO_HDRSIZE;
			ifc->hsize = hsize;
			ifc->connect = connect;
			ifc->name[0] = 0;
			ifc->wq = WR(q);
			ifc->ref = 1;
			unlock(ifc);
			return ifc;
		}
	}
	error(Enoifc);
}

/*
 *  Free an noifc.
 */
void
nonetfreeifc(Noifc *ifc)
{
	lock(ifc);
	ifc->ref--;
	if(ifc->ref == 0)
		ifc->wq = 0;
	unlock(ifc);
}

/*
 *  calculate the checksum of a list of blocks.  ignore the first `offset' bytes.
 */
int
nonetcksum(Block *bp, int offset)
{
	uchar *ep, *p;
	int n;
	ulong s;
	Nohdr *hp;

	s = 0;
	p = bp->rptr + offset;
	n = bp->wptr - p;
	hp = (Nohdr *)p;
	hp->sum[0] = hp->sum[1] = 0;
	for(;;){
		ep = p+(n&~0x7);
		while(p < ep) {
			s = s + s + p[0];
			s = s + s + p[1];
			s = s + s + p[2];
			s = s + s + p[3];
			s = s + s + p[4];
			s = s + s + p[5];
			s = s + s + p[6];
			s = s + s + p[7];
			s = (s&0xffff) + (s>>16);
			p += 8;
		}
		ep = p+(n&0x7);
		while(p < ep) {
			s = s + s + *p;
			p++;
		}
		s = (s&0xffff) + (s>>16);
		bp = bp->next;
		if(bp == 0)
			break;
		p = bp->rptr;
		n = BLEN(bp);
	}
	s = (s&0xffff) + (s>>16);
	hp->sum[1] = s>>8;
	hp->sum[0] = s;
	return s & 0xffff;
}

/*
 *  send acknowledges that need to be sent.  this happens at 1/2
 *  the retransmission interval.
 */
static void
nonetkproc(void *arg)
{
	Noifc *ifc;
	Noconv *cp, *ep;

	cp = 0;
	ifc = 0;
	if(waserror()){
		if(ifc)
			unlock(ifc);
		if(cp)
			qunlock(cp);
	}

loop:
	/*
	 *  loop through all active interfaces
	 */
	for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
		if(ifc->wq==0 || !canlock(ifc))
			continue;

		/*
		 *  loop through all active conversations
		 */
		ep = ifc->conv + conf.nnoconv;
		for(cp = ifc->conv; cp < ep; cp++){
			if(cp->state==Cclosed || !canqlock(cp))
				continue;
			if(cp->state == Cclosed){
				qunlock(cp);
				continue;
			}

			/*
			 *  resend the first message
			 */
			if(cp->first!=cp->next && cp->out[cp->first].time>=NOW){
				if(cp->rexmit++ > 100){
					print("hanging up\n");
					hangup(cp);
				} else
					sendmsg(cp, &(cp->out[cp->first]));
			}

			/*
			 *  resend an acknowledge
			 */
			if(cp->afirst != cp->anext){
				DPRINT("sending ack %d\n", cp->ack[cp->afirst]);
				sendctlmsg(cp, 0, 0);
			}
			qunlock(cp);
		}
		unlock(ifc);
	}
	tsleep(&nonetkr, return0, 0, MSrexmit/2);
	goto loop;
}

void
nonettoggle(void)
{
	pnonet ^= 1;
}

