#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

char	*ilstates[] = 
{ 
	"Closed",
	"Syncer",
	"Syncee",
	"Established",
	"Listening",
	"Closing" 
};

char	*iltype[] = 
{	
	"sync",
	"data",
	"dataquery",
	"ack",
	"query",
	"state",
	"close" 
};
static char *etime = "connection timed out";

enum				/* Packet types */
{
	Ilsync,
	Ildata,
	Ildataquery,
	Ilack,
	Ilquery,
	Ilstate,
	Ilclose,
};

enum				/* Connection state */
{
	Ilclosed,
	Ilsyncer,
	Ilsyncee,
	Ilestablished,
	Illistening,
	Ilclosing,
};

enum
{
	Nqt=	8,
};

typedef struct Ilcb Ilcb;
struct Ilcb			/* Control block */
{
	int	state;		/* Connection state */
	Conv	*conv;
	QLock	ackq;		/* Unacknowledged queue */
	Block	*unacked;
	Block	*unackedtail;
	ulong	unackeduchars;
	QLock	outo;		/* Out of order packet queue */
	Block	*outoforder;
	ulong	next;		/* Id of next to send */
	ulong	recvd;		/* Last packet received */
	ulong	start;		/* Local start id */
	ulong	rstart;		/* Remote start id */
	int	timeout;	/* Time out counter */
	int	slowtime;	/* Slow time counter */
	int	fasttime;	/* Retransmission timer */
	int	acktime;	/* Acknowledge timer */
	int	querytime;	/* Query timer */
	int	deathtime;	/* Time to kill connection */
	int	delay;		/* Average of the fixed rtt delay */
	int	rate;		/* Average uchar rate */
	int	mdev;		/* Mean deviation of rtt */
	int	maxrtt;		/* largest rtt seen */
	ulong	rttack;		/* The ack we are waiting for */
	int	rttlen;		/* Length of rttack packet */
	ulong	rttms;		/* Time we issued rttack packet */
	int	window;		/* Maximum receive window */
	int	rexmit;		/* number of retransmits */
	ulong	qt[Nqt+1];	/* state table for query messages */
	int	qtx;		/* ... index into qt */
};

enum
{
	IL_IPSIZE 	= 20,
	IL_HDRSIZE	= 18,	
	IL_LISTEN	= 0,
	IL_CONNECT	= 1,
	IP_ILPROTO	= 40,
};

typedef struct Ilhdr Ilhdr;
struct Ilhdr
{
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	cksum[2];	/* Header checksum */
	uchar	src[4];		/* Ip source */
	uchar	dst[4];		/* Ip destination */
	uchar	ilsum[2];	/* Checksum including header */
	uchar	illen[2];	/* Packet length */
	uchar	iltype;		/* Packet type */
	uchar	ilspec;		/* Special */
	uchar	ilsrc[2];	/* Src port */
	uchar	ildst[2];	/* Dst port */
	uchar	ilid[4];	/* Sequence id */
	uchar	ilack[4];	/* Acked sequence */
};

static struct Ilstats
{
	ulong	dup;
	ulong	dupb;
} ilstats;

/* Always Acktime < Fasttime < Slowtime << Ackkeepalive */
enum
{
	Seconds		= 1000,
	Iltickms 	= 100,		/* time base */

	Ackkeepalive	= 600*Seconds,
	Acktime		= 2*Iltickms,	/* max time twixt message rcvd & ack sent */

	Slowtime 	= 90*Seconds,	/* max time waiting for an ack before hangup */
	Fasttime 	= 4*Seconds,	/* max time between rexmit */
	Querytime	= 5*Seconds,	/* time between subsequent queries */
	Keepalivetime	= 60*Seconds,	/* time before first query */
	Deathtime	= 120*Seconds,	/* time between first query and hangup */
	Defaultwin	= 20,

	LogAGain	= 3,
	AGain		= 1<<LogAGain,
	LogDGain	= 2,
	DGain		= 1<<LogDGain,
	DefByteRate	= 1000,		/* 10 meg ether */
};

/* state for query/dataquery messages */


void	ilrcvmsg(Conv*, Block*);
void	ilsendctl(Conv*, Ilhdr*, int, ulong, ulong, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Conv*, Ilhdr*, Block*);
void	ilpullup(Conv*);
void	ilhangup(Conv*, char*);
void	ilfreeq(Ilcb*);
void	ilrexmit(Ilcb*);
void	ilbackoff(Ilcb*);
void	iltimers(Ilcb*);
char*	ilstart(Conv*, int, int);
void	ilackproc();
void	iloutoforder(Conv*, Ilhdr*, Block*);
void	iliput(uchar*, Block*);
void	iladvise(Block*, char*);
int	ilnextqt(Ilcb*);

#define DBG(x)	if((logmask & Logilmsg) && (iponly == 0 || x == iponly))netlog

	Proto	il;
	int 	ilcksum = 1;
static 	int 	initseq = 25001;
extern	Fs	fs;

static char*
ilconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	if(e != nil)
		return e;
	return ilstart(c, IL_CONNECT, 20);
}

static int
ilstate(Conv *c, char *state, int n)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return snprint(state, n, "%14.14s del %5.5d Br %5.5d md %5.5d una %5.5d rex %5.5d max %5.5d",
		ilstates[ic->state],
		ic->delay>>LogAGain, ic->rate>>LogAGain, ic->mdev>>LogDGain,
		ic->unackeduchars, ic->rexmit, ic->maxrtt);
}

static int
ilinuse(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return ic->state != Ilclosed;

}

/* called with c locked */
static char*
ilannounce(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil)
		return e;
	e = ilstart(c, IL_LISTEN, 20);
	if(e != nil)
		return e;
	Fsconnected(&fs, c, nil);

	return nil;
}

static void
ilclose(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)c->ptcl;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);

	switch(ic->state) {
	case Ilclosing:
	case Ilclosed:
		break;
	case Ilsyncer:
	case Ilsyncee:
	case Ilestablished:
		ic->state = Ilclosing;
		ilsendctl(c, nil, Ilclose, ic->next, ic->recvd, 0);
		break;
	case Illistening:
		ic->state = Ilclosed;
		ipmove(c->laddr, IPnoaddr);
		c->lport = 0;
		break;
	}
	ilfreeq(ic);
	unlock(c);
}

void
ilkick(Conv *c, int l)
{
	Ilhdr *ih;
	Ilcb *ic;
	int dlen;
	ulong id;
	Block *bp;

	USED(l);

	ic = (Ilcb*)c->ptcl;

	bp = qget(c->wq);
	if(bp == nil)
		return;

	switch(ic->state) {
	case Ilclosed:
	case Illistening:
	case Ilclosing:
		freeblist(bp);
		qhangup(c->rq, nil);
		return;
	}

	dlen = blocklen(bp);

	/* Make space to fit il & ip */
	bp = padblock(bp, IL_IPSIZE+IL_HDRSIZE);
	ih = (Ilhdr *)(bp->rp);

	/* Ip fields */
	ih->frag[0] = 0;
	ih->frag[1] = 0;
	v6tov4(ih->dst, c->raddr);
	v6tov4(ih->src, c->laddr);
	ih->proto = IP_ILPROTO;

	/* Il fields */
	hnputs(ih->illen, dlen+IL_HDRSIZE);
	hnputs(ih->ilsrc, c->lport);
	hnputs(ih->ildst, c->rport);

	qlock(&ic->ackq);
	id = ic->next++;
	hnputl(ih->ilid, id);

	hnputl(ih->ilack, ic->recvd);
	ih->iltype = Ildata;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	/* Checksum of ilheader plus data (not ip & no pseudo header) */
	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, IL_IPSIZE, dlen+IL_HDRSIZE));

	ilackq(ic, bp);
	qunlock(&ic->ackq);

	/* Start the round trip timer for this packet if the timer is free */
	if(ic->rttack == 0) {
		ic->rttack = id;
		ic->rttms = msec;
		ic->rttlen = dlen + IL_IPSIZE + IL_HDRSIZE;
	}
	ic->acktime = Ackkeepalive;

	ipoput(bp, 0, c->ttl);
}

static void
ilcreate(Conv *c)
{
	c->rq = qopen(64*1024, 0, 0, c);
	c->wq = qopen(64*1024, 0, 0, 0);
}

int
ilxstats(char *buf, int len)
{
	int n;

	n = snprint(buf, len,
		"il: csum %d hlen %d len %d order %d rexmit %d",
		il.csumerr, il.hlenerr, il.lenerr, il.order, il.rexmit);
	n += snprint(buf+n, len-n, " dupp %d dupb %d\n",
		ilstats.dup, ilstats.dupb);
	return n;
}

void
ilinit(Fs *fs)
{
	il.name = "il";
	il.kick = ilkick;
	il.connect = ilconnect;
	il.announce = ilannounce;
	il.state = ilstate;
	il.create = ilcreate;
	il.close = ilclose;
	il.rcv = iliput;
	il.ctl = nil;
	il.advise = iladvise;
	il.stats = ilxstats;
	il.inuse = ilinuse;
	il.ipproto = IP_ILPROTO;
	il.nc = Nchans;
	il.ptclsize = sizeof(Ilcb);

	kproc("ilack", ilackproc, 0);

	Fsproto(fs, &il);
}

void
ilackq(Ilcb *ic, Block *bp)
{
	Block *np;
	int n;

	n = blocklen(bp);

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyblock(bp, n);
	if(ic->unacked)
		ic->unackedtail->list = np;
	else {
		/* Start timer since we may have been idle for some time */
		iltimers(ic);
		ic->unacked = np;
	}
	ic->unackedtail = np;
	np->list = nil;
	ic->unackeduchars += n;
}

static
void
ilrttcalc(Ilcb *ic)
{
	int rtt, tt, pt, delay, rate;

	/* add in clock resolution hack */
	rtt = (msec + TK2MS(1) - 1) - ic->rttms;
	delay = ic->delay;
	rate = ic->rate;

	/* Guard against the ulong zero wrap of MACHP(0)->ticks */
	if(rtt > 120000)
		return;

	/* guess fixed delay as rtt of small packets */
	if(ic->rttlen < 128){
		delay += rtt - (delay>>LogAGain);
		if(delay < AGain)
			delay = AGain;
		ic->delay = delay;
	}

	/* rate */
	tt = rtt - (delay>>LogAGain);
	if(tt > 0){
		rate += ic->rttlen/tt - (rate>>LogAGain);
		if(rate < AGain)
			rate = AGain;
		ic->rate = rate;
	}

	/* mdev */
	pt = ic->rttlen/(rate>>LogAGain) + (delay>>LogAGain);
	ic->mdev += abs(rtt-pt) - (ic->mdev>>LogDGain);

	if(rtt > ic->maxrtt)
		ic->maxrtt = rtt;
}

void
ilackto(Ilcb *ic, ulong ackto)
{
	Ilhdr *h;
	Block *bp;
	ulong id;

	if(ic->rttack == ackto)
		ilrttcalc(ic);

	/* Cancel if we've passed the packet we were interested in */
	if(ic->rttack <= ackto)
		ic->rttack = 0;

	qlock(&ic->ackq);
	while(ic->unacked) {
		h = (Ilhdr *)ic->unacked->rp;
		id = nhgetl(h->ilid);
		if(ackto < id)
			break;

		bp = ic->unacked;
		ic->unacked = bp->list;
		bp->list = nil;
		ic->unackeduchars -= blocklen(bp);
		freeblist(bp);
	}
	qunlock(&ic->ackq);
}

void
iliput(uchar*, Block *bp)
{
	char *st;
	Ilcb *ic;
	Ilhdr *ih;
	uchar raddr[IPaddrlen];
	uchar laddr[IPaddrlen];
	ushort sp, dp, csum;
	int plen, illen;
	Conv *s, **p, *new, *spec, *gen;

	ih = (Ilhdr *)bp->rp;
	plen = blocklen(bp);
	if(plen < IL_IPSIZE+IL_HDRSIZE){
		netlog(Logil, "il: hlenerr\n");
		il.hlenerr++;
		goto raise;
	}

	illen = nhgets(ih->illen);
	if(illen+IL_IPSIZE > plen){
		netlog(Logil, "il: lenerr\n");
		il.lenerr++;
		goto raise;
	}

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	v4tov6(raddr, ih->src);

	if(ilcksum && (csum = ptclcsum(bp, IL_IPSIZE, illen)) != 0) {
		if(ih->iltype < 0 || ih->iltype > Ilclose)
			st = "?";
		else
			st = iltype[ih->iltype];
		il.csumerr++;
		netlog(Logil, "il: cksum %ux %ux, pkt(%s id %lud ack %lud %I/%d->%d)\n",
			csum, st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp);
		goto raise;
	}

	for(p = il.conv; *p; p++) {
		s = *p;
		if(s->lport == sp)
		if(s->rport == dp)
		if(ipcmp(s->raddr, raddr) == 0) {
			ilprocess(s, ih, bp);
			return;
		}
	}

	if(ih->iltype != Ilsync){
		if(ih->iltype < 0 || ih->iltype > Ilclose)
			st = "?";
		else
			st = iltype[ih->iltype];
		netlog(Logil, "il: no channel, pkt(%s id %lud ack %lud %I/%ud->%ud)\n",
			st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp); 
		goto raise;
	}

	gen = nil;
	spec = nil;
	for(p = il.conv; *p; p++) {
		s = *p;
		ic = (Ilcb*)s->ptcl;
		if(ic->state != Illistening)
			continue;

		if(s->rport == 0 && ipcmp(s->raddr, IPnoaddr) == 0) {
			if(s->lport == sp) {
				spec = s;
				break;
			}
			if(s->lport == 0)
				gen = s;
		}
	}

	if(spec)
		s = spec;
	else
	if(gen)
		s = gen;
	else
		goto raise;

	v4tov6(laddr, ih->dst);
	new = Fsnewcall(&fs, s, raddr, dp, laddr, sp);
	if(new == nil){
		netlog(Logil, "il: bad newcall %I/%ud->%ud\n", raddr, sp, dp);
		ilsendctl(nil, ih, Ilclose, 0, nhgetl(ih->ilid), 0);
		goto raise;
	}

	ic = (Ilcb*)new->ptcl;
	ic->conv = new;
	ic->state = Ilsyncee;
	initseq += msec;
	ic->start = initseq & 0xffffff;
	ic->next = ic->start+1;
	ic->recvd = 0;
	ic->rstart = nhgetl(ih->ilid);
	ic->slowtime = Slowtime;
	ic->delay = Iltickms<<LogAGain;
	ic->mdev = Iltickms<<LogDGain;
	ic->rate = DefByteRate<<LogAGain;
	ic->querytime = Keepalivetime;
	ic->deathtime = Deathtime;
	ic->window = Defaultwin;

	ilprocess(new, ih, bp);
	return;

raise:
	freeblist(bp);
	return;
}

void
_ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	ulong id, ack;

	id = nhgetl(h->ilid);
	ack = nhgetl(h->ilack);

	ic = (Ilcb*)s->ptcl;

	ic->querytime = Keepalivetime;
	ic->deathtime = Deathtime;

	switch(ic->state) {
	default:
		netlog(Logil, "il: unknown state %d\n", ic->state);
	case Ilclosed:
		freeblist(bp);
		break;
	case Ilsyncer:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(ack != ic->start)
				ilhangup(s, "connection rejected");
			else {
				ic->recvd = id;
				ic->rstart = id;
				ilsendctl(s, nil, Ilack, ic->next, ic->recvd, 0);
				ic->state = Ilestablished;
				Fsconnected(&fs, s, nil);
				ilpullup(s);
				iltimers(ic);
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "remote close");
			break;
		}
		freeblist(bp);
		break;
	case Ilsyncee:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(id != ic->rstart || ack != 0)
				ic->state = Ilclosed;
			else {
				ic->recvd = id;
				ilsendctl(s, nil, Ilsync, ic->start, ic->recvd, 0);
				iltimers(ic);
			}
			break;
		case Ilack:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ilpullup(s);
				iltimers(ic);
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "remote close");
			break;
		}
		freeblist(bp);
		break;
	case Ilestablished:
		switch(h->iltype) {
		case Ilsync:
			if(id != ic->rstart)
				ilhangup(s, "remote close");
			else {
				ilsendctl(s, nil, Ilack, ic->next, ic->rstart, 0);
				iltimers(ic);
			}
			freeblist(bp);	
			break;
		case Ildata:
			iltimers(ic);
			ilackto(ic, ack);
			ic->acktime = Acktime;
			iloutoforder(s, h, bp);
			ilpullup(s);
			break;
		case Ildataquery:
			iltimers(ic);
			ilackto(ic, ack);
			ic->acktime = Acktime;
			iloutoforder(s, h, bp);
			ilpullup(s);
			ilsendctl(s, nil, Ilstate, ic->next, ic->recvd, h->ilspec);
			break;
		case Ilack:
			ilackto(ic, ack);
			iltimers(ic);
			freeblist(bp);
			break;
		case Ilquery:
			ilackto(ic, ack);
			ilsendctl(s, nil, Ilstate, ic->next, ic->recvd, h->ilspec);
			iltimers(ic);
			freeblist(bp);
			break;
		case Ilstate:
			ilackto(ic, ack);
			if(h->ilspec > Nqt)
				h->ilspec = 0;
			if(ic->qt[h->ilspec] > ack)
				ilrexmit(ic);
			iltimers(ic);
			freeblist(bp);
			break;
		case Ilclose:
			freeblist(bp);
			if(ack < ic->start || ack > ic->next) 
				break;
			ilsendctl(s, nil, Ilclose, ic->next, ic->recvd, 0);
			ic->state = Ilclosing;
			ilfreeq(ic);
			iltimers(ic);
			break;
		}
		break;
	case Illistening:
		freeblist(bp);
		break;
	case Ilclosing:
		switch(h->iltype) {
		case Ilclose:
			ic->recvd = id;
			ilsendctl(s, nil, Ilclose, ic->next, ic->recvd, 0);
			if(ack == ic->next)
				ilhangup(s, nil);
			iltimers(ic);
			break;
		default:
			break;
		}
		freeblist(bp);
		break;
	}
}

void
ilrexmit(Ilcb *ic)
{
	Ilhdr *h;
	Block *nb;
	Conv *c;
	ulong id;
	int x;

	nb = nil;
	qlock(&ic->ackq);
	if(ic->unacked)
		nb = copyblock(ic->unacked, blocklen(ic->unacked));
	qunlock(&ic->ackq);

	if(nb == nil)
		return;

	h = (Ilhdr*)nb->rp;

	h->iltype = Ildataquery;
	hnputl(h->ilack, ic->recvd);
	h->ilspec = ilnextqt(ic);
	h->ilsum[0] = 0;
	h->ilsum[1] = 0;
	if(ilcksum)
		hnputs(h->ilsum, ptclcsum(nb, IL_IPSIZE, nhgets(h->illen)));

	c = ic->conv;
	id = nhgetl(h->ilid);
	netlog(Logil, "il: rexmit %ud %ud: %d %d: %i %d/%d\n", id, ic->recvd,
		ic->fasttime, ic->timeout,
		c->raddr, c->lport, c->rport);

	il.rexmit++;
	ic->rexmit++;

	/*
	 *  Double delay estimate and half bandwidth estimate.  This is
	 *  in keeping with van jacobson's tcp alg.
	 */
	ic->rttack = 0;
	if((ic->delay>>LogAGain) < 2*Seconds)
		ic->delay *= 2;
	x = ic->rate>>1;
	if(x >= (1<<LogAGain))
		ic->rate = x;

	ipoput(nb, 0, ic->conv->ttl);
}

/* DEBUG */
void
ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;

	ic = (Ilcb*)s->ptcl;

	USED(ic);
	DBG(s->raddr)(Logilmsg, "%11s rcv %d/%d snt %d/%d pkt(%s id %d ack %d %d->%d) ",
		ilstates[ic->state],  ic->rstart, ic->recvd, ic->start, 
		ic->next, iltype[h->iltype], nhgetl(h->ilid), 
		nhgetl(h->ilack), nhgets(h->ilsrc), nhgets(h->ildst));

	_ilprocess(s, h, bp);

	DBG(s->raddr)(Logilmsg, "%11s rcv %d snt %d\n", ilstates[ic->state], ic->recvd, ic->next);
}

void
ilhangup(Conv *s, char *msg)
{
	Ilcb *ic;
	int callout;

	netlog(Logil, "il: hangup! %I %d/%d: %s\n", s->raddr, s->lport, s->rport, msg?msg:"no reason");

	ic = (Ilcb*)s->ptcl;
	callout = ic->state == Ilsyncer;
	ic->state = Ilclosed;

	qhangup(s->rq, msg);
	qhangup(s->wq, msg);

	if(callout)
		Fsconnected(&fs, s, msg);
}

void
ilpullup(Conv *s)
{
	Ilcb *ic;
	Ilhdr *oh;
	Block *bp;
	ulong oid, dlen;

	ic = (Ilcb*)s->ptcl;
	if(ic->state != Ilestablished)
		return;

	qlock(&ic->outo);
	while(ic->outoforder) {
		bp = ic->outoforder;
		oh = (Ilhdr*)bp->rp;
		oid = nhgetl(oh->ilid);
		if(oid <= ic->recvd) {
			ic->outoforder = bp->list;
			freeblist(bp);
			continue;
		}
		if(oid != ic->recvd+1){
			il.order++;
			break;
		}

		ic->recvd = oid;
		ic->outoforder = bp->list;

		bp->list = nil;
		dlen = nhgets(oh->illen)-IL_HDRSIZE;
		bp = trimblock(bp, IL_IPSIZE+IL_HDRSIZE, dlen);
		/*
		 * Upper levels don't know about multiple-block
		 * messages so copy all into one (yick).
		 */
		bp = concatblock(bp);
		if(bp == 0)
			panic("ilpullup");
		qpass(s->rq, bp);
	}
	qunlock(&ic->outo);
}

void
iloutoforder(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	uchar *lid;
	Block *f, **l;
	ulong id, newid;

	ic = (Ilcb*)s->ptcl;
	bp->list = nil;

	id = nhgetl(h->ilid);
	/* Window checks */
	if(id <= ic->recvd || id > ic->recvd+ic->window) {
		netlog(Logil, "il: message outside window %ud <%ud-%ud>: %i %d/%d\n",
			id, ic->recvd, ic->recvd+ic->window, s->raddr, s->lport, s->rport);
		freeblist(bp);
		return;
	}

	/* Packet is acceptable so sort onto receive queue for pullup */
	qlock(&ic->outo);
	if(ic->outoforder == nil)
		ic->outoforder = bp;
	else {
		l = &ic->outoforder;
		for(f = *l; f; f = f->list) {
			lid = ((Ilhdr*)(f->rp))->ilid;
			newid = nhgetl(lid);
			if(id <= newid) {
				if(id == newid) {
					ilstats.dup++;
					ilstats.dupb += blocklen(bp);
					qunlock(&ic->outo);
					freeblist(bp);
					return;
				}
				bp->list = f;
				*l = bp;
				qunlock(&ic->outo);
				return;
			}
			l = &f->list;
		}
		*l = bp;
	}
	qunlock(&ic->outo);
}

void
ilsendctl(Conv *ipc, Ilhdr *inih, int type, ulong id, ulong ack, int ilspec)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;
	int ttl;

	bp = allocb(IL_IPSIZE+IL_HDRSIZE);
	bp->wp += IL_IPSIZE+IL_HDRSIZE;

	ih = (Ilhdr *)(bp->rp);

	/* Ip fields */
	ih->proto = IP_ILPROTO;
	hnputs(ih->illen, IL_HDRSIZE);
	ih->frag[0] = 0;
	ih->frag[1] = 0;
	if(inih) {
		hnputl(ih->dst, nhgetl(inih->src));
		hnputl(ih->src, nhgetl(inih->dst));
		hnputs(ih->ilsrc, nhgets(inih->ildst));
		hnputs(ih->ildst, nhgets(inih->ilsrc));
		hnputl(ih->ilid, nhgetl(inih->ilack));
		hnputl(ih->ilack, nhgetl(inih->ilid));
		ttl = MAXTTL;
	}
	else {
		v6tov4(ih->dst, ipc->raddr);
		v6tov4(ih->src, ipc->laddr);
		hnputs(ih->ilsrc, ipc->lport);
		hnputs(ih->ildst, ipc->rport);
		hnputl(ih->ilid, id);
		hnputl(ih->ilack, ack);
		ic = (Ilcb*)ipc->ptcl;
		ic->acktime = Ackkeepalive;
		ttl = ipc->ttl;
	}
	ih->iltype = type;
	ih->ilspec = ilspec;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, IL_IPSIZE, IL_HDRSIZE));

	if(ipc){
		DBG(ipc->raddr)(Logilmsg, "ctl(%s id %d ack %d %d->%d)\n",
		iltype[ih->iltype], nhgetl(ih->ilid), nhgetl(ih->ilack), 
		nhgets(ih->ilsrc), nhgets(ih->ildst));
	}

	ipoput(bp, 0, ttl);
}

void
ilackproc()
{
	Ilcb *ic;
	Conv **s, *p;
	static Rendez ilr;

loop:
	tsleep(&ilr, return0, 0, Iltickms);
	for(s = il.conv; s && *s; s++) {
		p = *s;
		ic = (Ilcb*)p->ptcl;

		ic->timeout += Iltickms;
		switch(ic->state) {
		case Ilclosed:
		case Illistening:
			break;
		case Ilclosing:
			if(ic->timeout >= ic->fasttime) {
				ilsendctl(p, nil, Ilclose, ic->next, ic->recvd, 0);
				ilbackoff(ic);
			}
			if(ic->timeout >= ic->slowtime)
				ilhangup(p, nil);
			break;
		case Ilsyncee:
		case Ilsyncer:
			if(ic->timeout >= ic->fasttime) {
				ilsendctl(p, nil, Ilsync, ic->start, ic->recvd, 0);
				ilbackoff(ic);
			}
			if(ic->timeout >= ic->slowtime)
				ilhangup(p, etime);
			break;
		case Ilestablished:
			ic->acktime -= Iltickms;
			if(ic->acktime <= 0)
				ilsendctl(p, nil, Ilack, ic->next, ic->recvd, 0);

			ic->querytime -= Iltickms;
			if(ic->querytime <= 0){
				ic->deathtime -= Querytime;
				if(ic->deathtime < 0){
					netlog(Logil, "il: hangup due to deathtime (%d) < 0 \n", ic->deathtime);
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, nil, Ilquery, ic->next, ic->recvd, ilnextqt(ic));
				ic->querytime = Querytime;
			}
			if(ic->unacked == nil) {
				ic->timeout = 0;
				break;
			}
			if(ic->timeout >= ic->fasttime) {
				ilrexmit(ic);
				ilbackoff(ic);
			}
			if(ic->timeout >= ic->slowtime) {
				netlog(Logil, "il: hangup due to timeout (%d) >= slowtime (%d)\n", ic->timeout, ic->slowtime);
				ilhangup(p, etime);
				break;
			}
			break;
		}
	}
	goto loop;
}

void
ilbackoff(Ilcb *ic)
{
	ic->fasttime += ic->fasttime>>1;
}

char*
ilstart(Conv *c, int type, int window)
{
	char *e;
	Ilcb *ic;

	ic = (Ilcb*)c->ptcl;
	ic->conv = c;

	e = nil;

	if(ic->state != Ilclosed)
		return e;

	ic->unacked = nil;
	ic->outoforder = nil;
	ic->unackeduchars = 0;
	ic->delay = Iltickms<<LogAGain;
	ic->mdev = Iltickms<<LogDGain;
	ic->rate = DefByteRate<<LogAGain;
	iltimers(ic);

	initseq += msec;
	ic->start = initseq & 0xffffff;
	ic->next = ic->start+1;
	ic->recvd = 0;
	ic->window = window;
	ic->rexmit = 0;
	ic->qtx = 1;

	switch(type) {
	default:
		netlog(Logil, "il: start: type %d\n", type);
		break;
	case IL_LISTEN:
		ic->state = Illistening;
		break;
	case IL_CONNECT:
		ic->state = Ilsyncer;
		ilsendctl(c, nil, Ilsync, ic->start, ic->recvd, 0);
		break;
	}

	return e;
}

void
ilfreeq(Ilcb *ic)
{
	Block *bp, *next;

	qlock(&ic->ackq);
	for(bp = ic->unacked; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->unacked = nil;
	qunlock(&ic->ackq);

	qlock(&ic->outo);
	for(bp = ic->outoforder; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->outoforder = nil;
	qunlock(&ic->outo);
}

void
iltimers(Ilcb *ic)
{
	int pt;

	ic->timeout = 0;
	pt = (ic->delay>>LogAGain) + ic->unackeduchars/(ic->rate>>LogAGain) + ic->mdev;
	ic->fasttime = Acktime + pt + Iltickms - 1;
	if(ic->fasttime > Fasttime)
		ic->fasttime = Fasttime;
	ic->slowtime = (Slowtime/Seconds)*pt;
	if(ic->slowtime < Slowtime)
		ic->slowtime = Slowtime;
}

void
iladvise(Block *bp, char *msg)
{
	Ilhdr *h;
	Ilcb *ic;		
	uchar source[IPaddrlen], dest[IPaddrlen];
	ushort psource;
	Conv *s, **p;

	h = (Ilhdr*)(bp->rp);

	v4tov6(dest, h->dst);
	v4tov6(source, h->src);
	psource = nhgets(h->ilsrc);


	/* Look for a connection, unfortunately the destination port is missing */
	for(p = il.conv; *p; p++) {
		s = *p;
		if(s->lport == psource)
		if(ipcmp(s->laddr, source) == 0)
		if(ipcmp(s->raddr, dest) == 0){
			ic = (Ilcb*)s->ptcl;
			switch(ic->state){
			case Ilsyncer:
				ilhangup(s, msg);
				break;
			}
			break;
		}
	}
	freeblist(bp);
}

int
ilnextqt(Ilcb *ic)
{
	int x;

	qlock(&ic->ackq);
	x = ic->qtx;
	ic->qt[x] = ic->next-1;	/* highest xmitted packet */
	ic->qt[0] = ic->qt[x];	/* compatibility with old implementations */
	if(++x > Nqt)
		x = 1;
	ic->qtx = x;
	qunlock(&ic->ackq);

	return x;
}
