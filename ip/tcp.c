#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

enum
{
	QMAX		= 64*1024-1,
	IP_TCPPROTO	= 6,
	TCP_IPLEN	= 8,
	TCP_PHDRSIZE	= 12,
	TCP_HDRSIZE	= 20,
	TCP_PKT		= TCP_IPLEN+TCP_PHDRSIZE,
	TimerOFF	= 0,
	TimerON		= 1,
	TimerDONE	= 2,
	MAX_TIME 	= (1<<20),	/* Forever */
	TCP_ACK		= 50,		/* Timed ack sequence in ms */

	URG		= 0x20,		/* Data marked urgent */
	ACK		= 0x10,		/* Acknowledge is valid */
	PSH		= 0x08,		/* Whole data pipe is pushed */
	RST		= 0x04,		/* Reset connection */
	SYN		= 0x02,		/* Pkt. is synchronise */
	FIN		= 0x01,		/* Start close down */

	EOLOPT		= 0,
	NOOPOPT		= 1,
	MAXBACKOFF	= 20,
	MSSOPT		= 2,
	MSS_LENGTH	= 4,		/* Mean segment size */
	MSL2		= 10,
	MSPTICK		= 50,		/* Milliseconds per timer tick */
	DEF_MSS		= 1024,		/* Default mean segment */
	DEF_RTT		= 150,		/* Default round trip */
	TCP_LISTEN	= 0,		/* Listen connection */
	TCP_CONNECT	= 1,		/* Outgoing connection */

	FORCE		= 1,
	CLONE		= 2,
	RETRAN		= 4,
	ACTIVE		= 8,
	SYNACK		= 16,
	ACKED		= 32,

	LOGAGAIN	= 3,
	LOGDGAIN	= 2,
	Closed		= 0,		/* Connection states */
	Listen,
	Syn_sent,
	Syn_received,
	Established,
	Finwait1,
	Finwait2,
	Close_wait,
	Closing,
	Last_ack,
	Time_wait
};

/* Must correspond to the enumeration above */
char *tcpstates[] =
{
	"Closed", 	"Listen", 	"Syn_sent", "Syn_received",
	"Established", 	"Finwait1",	"Finwait2", "Close_wait",
	"Closing", 	"Last_ack", 	"Time_wait"
};

typedef struct Timer Timer;
struct Timer
{
	Timer	*next;
	Timer	*prev;
	int	state;
	int	start;
	int	count;
	void	(*func)(void*);
	void	*arg;
};

typedef struct Tcphdr Tcphdr;
struct Tcphdr
{
	byte	vihl;		/* Version and header length */
	byte	tos;		/* Type of service */
	byte	length[2];	/* packet length */
	byte	id[2];		/* Identification */
	byte	frag[2];	/* Fragment information */
	byte	Unused;
	byte	proto;
	byte	tcplen[2];
	byte	tcpsrc[4];
	byte	tcpdst[4];
	byte	tcpsport[2];
	byte	tcpdport[2];
	byte	tcpseq[4];
	byte	tcpack[4];
	byte	tcpflag[2];
	byte	tcpwin[2];
	byte	tcpcksum[2];
	byte	tcpurg[2];
	/* Options segment */
	byte	tcpopt[2];
	byte	tcpmss[2];
};

typedef struct Tcp Tcp;
struct	Tcp
{
	ushort	source;
	ushort	dest;
	ulong	seq;
	ulong	ack;
	byte	flags;
	ushort	wnd;
	ushort	urg;
	ushort	mss;
};

typedef struct Reseq Reseq;
struct Reseq
{
	Reseq 	*next;
	Tcp	seg;
	Block	*bp;
	ushort	length;
};

typedef struct Tcpctl Tcpctl;
struct Tcpctl
{
	QLock;
	byte	state;			/* Connection state */
	byte	type;			/* Listening or active connection */
	byte	code;			/* Icmp code */
	struct {
		ulong	una;		/* Unacked data pointer */
		ulong	nxt;		/* Next sequence expected */
		ulong	ptr;		/* Data pointer */
		ushort	wnd;		/* Tcp send window */
		ulong	urg;		/* Urgent data pointer */
		ulong	wl1;
		ulong	wl2;
	} snd;
	struct {
		ulong	nxt;		/* Receive pointer to next byte slot */
		ushort	wnd;		/* Receive window incoming */
		ulong	urg;		/* Urgent pointer */
		int	blocked;
	} rcv;
	ulong	iss;			/* Initial sequence number */
	ushort	cwind;			/* Congestion window */
	ushort	ssthresh;		/* Slow start threshold */
	int	resent;			/* Bytes just resent */
	int	irs;			/* Initial received squence */
	ushort	mss;			/* Mean segment size */
	int	rerecv;			/* Overlap of data rerecevived */
	ushort	window;			/* Recevive window */
	int	max_snd;		/* Max send */
	ulong	last_ack;		/* Last acknowledege received */
	byte	backoff;		/* Exponential backoff counter */
	byte	flags;			/* State flags */
	ulong	sndcnt;			/* Amount of data in send queue */
	Reseq	*reseq;			/* Resequencing queue */
	Timer	timer;			/* Activity timer */
	Timer	acktimer;		/* Acknowledge timer */
	Timer	rtt_timer;		/* Round trip timer */
	ulong	rttseq;			/* Round trip sequence */
	int	srtt;			/* Shortened round trip */
	int	mdev;			/* Mean deviation of round trip */
	int	kacounter;		/* count down for keep alive */
	int	f2counter;		/* count down for finwait2 state */
	uint	sndsyntime;		/* time syn sent */
	char	ascstate[128];
};

#define DBG(x)	if((logmask & Logtcpmsg) && (iponly == 0 || x == iponly))netlog

Proto	tcp;
int	tcp_irtt = DEF_RTT;	/* Initial guess at round trip time */
ushort	tcp_mss  = DEF_MSS;	/* Maximum segment size to be sent */
Timer 	*timers;		/* List of active timers */
QLock 	tl;			/* Protect timer list */

void	addreseq(Tcpctl*, Tcp*, Block*, ushort);
void	getreseq(Tcpctl*, Tcp*, Block**, ushort*);
void	localclose(Conv*, char*);
void	procsyn(Conv*, Tcp*);
void	tcpiput(Block*);
void	tcpoutput(Conv*);
int	tcptrim(Tcpctl*, Tcp*, Block**, ushort*);
void	tcpstart(Conv*, int, ushort);
void	tcptimeout(void*);
void	tcpsndsyn(Tcpctl*);
void	tcprcvwin(Conv*);
void	tcpacktimer(Conv*);

void
tcpsetstate(Conv *s, byte newstate)
{
	Tcpctl *tcb;
	byte oldstate;

	tcb = (Tcpctl*)s->ptcl;

	oldstate = tcb->state;
	if(oldstate == newstate)
		return;

	/*
	  print("%d/%d %s->%s\n", s->lport, s->rport,
			tcpstates[oldstate], tcpstates[newstate]);
	/**/

	tcb->state = newstate;

	switch(newstate) {
	case Closed:
		qclose(s->rq);
		qclose(s->wq);
		qclose(s->eq);
		s->lport = 0;		/* This connection is toast */
		s->rport = 0;
		s->raddr = 0;

	case Close_wait:		/* Remote closes */
		qhangup(s->rq, nil);
		break;
	}

	if(oldstate == Syn_sent)
		Fsconnected(&fs, s, nil);
}

static char*
tcpconnect(Conv *c, char **argv, int argc)
{
	char *rv;

	rv = Fsstdconnect(c, argv, argc);
	if(rv)
		return rv;
	tcpstart(c, TCP_CONNECT, QMAX);
	return rv;
}

int
tcpstate(char **msg, Conv *c)
{
	Tcpctl *s;
	int isclose;

	s = (Tcpctl*)(c->ptcl);

	isclose = 0;
	if(s->state == Closed)
		isclose = 1;

/*	snprint(s->ascstate, sizeof(s->ascstate),
		"tcnt %d tnxt %lux tuna %lux twnd %d rack %lux rnxt %lux rwnd %d",
		s->sndcnt, s->snd.nxt, s->snd.una, s->snd.wnd,
		s->last_ack, s->rcv.nxt, s->rcv.wnd);*/
	snprint(s->ascstate, sizeof(s->ascstate),
		"%s srtt %d mdev %d timer.start %d timer.count %d\n",
		tcpstates[s->state], s->srtt, s->mdev,
		s->timer.start, s->timer.count);

	*msg = s->ascstate;
	return isclose;
}

static void
tcpannounce(Conv *c)
{
	tcpstart(c, TCP_LISTEN, QMAX);
	Fsconnected(&fs, c, nil);
}

static void
tcpclose(Conv *c)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)c->ptcl;

	qhangup(c->rq, nil);
	qhangup(c->wq, nil);
	qhangup(c->eq, nil);

	unlock(c);

	switch(tcb->state) {
	case Listen:
		/*
		 *  reset any incoming calls to this listener
		 */
		Fsconnected(&fs, c, "Hangup");

		qlock(tcb);
		localclose(c, nil);
		break;
	case Closed:
	case Syn_sent:
		qlock(tcb);
		localclose(c, nil);
		break;
	case Syn_received:
	case Established:
		qlock(tcb);
		tcb->sndcnt++;
		tcb->snd.nxt++;
		tcpsetstate(c, Finwait1);
		tcpoutput(c);
		break;
	case Close_wait:
		qlock(tcb);
		tcb->sndcnt++;
		tcb->snd.nxt++;
		tcpsetstate(c, Last_ack);
		tcpoutput(c);
		break;
	}
	qunlock(tcb);
}

void
tcpkick(Conv *s, int len)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;


	switch(tcb->state) {
	case Listen:
		tcb->flags |= ACTIVE;
		tcpsndsyn(tcb);
		tcpsetstate(s, Syn_sent);
		/* No break */
	case Syn_sent:
	case Syn_received:
	case Established:
	case Close_wait:
		/*
		 * Push data
		 */
		qlock(tcb);
		tcb->sndcnt += len;
		tcprcvwin(s);
		tcpoutput(s);
		qunlock(tcb);
		break;
	default:
		localclose(s, "Hangup");
	}
}

/*
 *  get remote sender going if it was flow controlled due to a closed window
 */
static void
deltimer(Timer *t)
{
	if(timers == t)
		timers = t->next;
	if(t->next)
		t->next->prev = t->prev;
	if(t->prev)
		t->prev->next = t->next;
}

void
tcprcvwin(Conv *s)				/* Call with tcb locked */
{
	int w;
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;
	w = QMAX - qlen(s->rq);
	if(w < 0)
		w = 0;
	tcb->rcv.wnd = w;
	if(w == 0)
		tcb->rcv.blocked = 1;
}

void
tcpacktimer(Conv *s)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;

	qlock(tcb);
	tcb->flags |= FORCE;
	tcprcvwin(s);
	tcpoutput(s);
	qunlock(tcb);
}

static void
tcpcreate(Conv *c)
{
	c->rq = qopen(QMAX, 0, tcpacktimer, c);
	c->wq = qopen(QMAX, 0, 0, 0);
}

void
tcpackproc(void*)
{
	Timer *t, *tp, *timeo;
	static Rendez tcpr;

	for(;;) {
		tsleep(&tcpr, return0, 0, MSPTICK);

		qlock(&tl);
		timeo = nil;
		for(t = timers; t != nil; t = tp) {
			tp = t->next;
 			if(t->state == TimerON) {
				t->count--;
				if(t->count == 0) {
					deltimer(t);
					t->state = TimerDONE;
					t->next = timeo;
					timeo = t;
				}
			}
		}
		qunlock(&tl);

		for(;;) {
			t = timeo;
			if(t == nil)
				break;

			timeo = t->next;
			if(t->state == TimerDONE && t->func != nil)
				(*t->func)(t->arg);
		}
	}
}

void
tcpgo(Timer *t)
{
	if(t == nil || t->start == 0)
		return;

	qlock(&tl);
	t->count = t->start;
	if(t->state != TimerON) {
		t->state = TimerON;
		t->prev = nil;
		t->next = timers;
		if(t->next)
			t->next->prev = t;
		timers = t;
	}
	qunlock(&tl);
}

void
tcphalt(Timer *t)
{
	if(t == nil)
		return;

	qlock(&tl);
	if(t->state == TimerON)
		deltimer(t);
	t->state = TimerOFF;
	qunlock(&tl);
}

int
backoff(int n)
{
	if(n < 5)
		return 1 << n;

	return 64;
}

void
localclose(Conv *s, char *reason)	 /*  called with tcb locked */
{
	Tcpctl *tcb;
	Reseq *rp,*rp1;

	tcb = (Tcpctl*)s->ptcl;

	tcphalt(&tcb->timer);
	tcphalt(&tcb->rtt_timer);

	/* Flush reassembly queue; nothing more can arrive */
	for(rp = tcb->reseq; rp != nil; rp = rp1) {
		rp1 = rp->next;
		freeblist(rp->bp);
		free(rp);
	}

	if(tcb->state == Syn_sent)
		Fsconnected(&fs, s, reason);

	qhangup(s->rq, reason);
	qhangup(s->wq, reason);

	tcb->reseq = nil;
	tcpsetstate(s, Closed);
}

void
inittcpctl(Conv *s)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;

	memset(tcb, 0, sizeof(Tcpctl));

	tcb->cwind = tcp_mss;
	tcb->mss = tcp_mss;
	tcb->ssthresh = 65535;
	tcb->srtt = 0;

	tcb->timer.start = tcp_irtt / MSPTICK;
	tcb->timer.func = tcptimeout;
	tcb->timer.arg = s;
	tcb->rtt_timer.start = MAX_TIME;
	tcb->acktimer.start = TCP_ACK / MSPTICK;
	tcb->acktimer.func = tcpacktimer;
	tcb->acktimer.arg = s;
}

/* mtu (- TCP + IP hdr len) of 1st hop */
int
tcpmtu(Conv *s)
{
	Media *m;
	byte dst[4], dummy[4];
	int mtu;

	mtu = 0;
	hnputl(dst, s->raddr);
	m = Mediaroute(dst, dummy);
	if(m != nil)
		mtu = m->maxmtu - m->hsize - (TCP_PKT + TCP_HDRSIZE);
	if(mtu < 4)
		mtu = DEF_MSS;
	return mtu;
}

void
tcpstart(Conv *s, int mode, ushort window)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;

	inittcpctl(s);
	tcb->window = window;
	tcb->rcv.wnd = window;

	switch(mode) {
	case TCP_LISTEN:
		tcb->flags |= CLONE;
		tcpsetstate(s, Listen);
		break;

	case TCP_CONNECT:
		/* Send SYN, go into SYN_SENT state */
		qlock(tcb);
		tcb->flags |= ACTIVE;
		tcpsndsyn(tcb);
		tcpsetstate(s, Syn_sent);
		tcpoutput(s);
		qunlock(tcb);
		break;
	}
}

static char*
tcpflag(ushort flag)
{
	static char buf[128];

	sprint(buf, "%d", flag>>10);	/* Head len */
	if(flag & URG)
		strcat(buf, " URG");
	if(flag & ACK)
		strcat(buf, " ACK");
	if(flag & PSH)
		strcat(buf, " PSH");
	if(flag & RST)
		strcat(buf, " RST");
	if(flag & SYN)
		strcat(buf, " SYN");
	if(flag & FIN)
		strcat(buf, " FIN");

	return buf;
}

Block *
htontcp(Tcp *tcph, Block *data, Tcphdr *ph)
{
	int dlen;
	Tcphdr *h;
	ushort csum;
	ushort hdrlen;

	hdrlen = TCP_HDRSIZE;
	if(tcph->mss)
		hdrlen += MSS_LENGTH;

	if(data) {
		dlen = blocklen(data);
		data = padblock(data, hdrlen + TCP_PKT);
		if(data == nil)
			return nil;
	}
	else {
		dlen = 0;
		data = allocb(hdrlen + TCP_PKT);
		if(data == nil)
			return nil;
		data->wp += hdrlen + TCP_PKT;
	}


	memmove(data->rp, ph, TCP_PKT);

	h = (Tcphdr *)(data->rp);
	h->proto = IP_TCPPROTO;
	h->frag[0] = 0;
	h->frag[1] = 0;
	hnputs(h->tcplen, hdrlen + dlen);
	hnputs(h->tcpsport, tcph->source);
	hnputs(h->tcpdport, tcph->dest);
	hnputl(h->tcpseq, tcph->seq);
	hnputl(h->tcpack, tcph->ack);
	hnputs(h->tcpflag, (hdrlen<<10) | tcph->flags);
	hnputs(h->tcpwin, tcph->wnd);
	h->tcpcksum[0] = 0;
	h->tcpcksum[1] = 0;
	h->Unused = 0;
	hnputs(h->tcpurg, tcph->urg);

	if(tcph->mss != 0){
		h->tcpopt[0] = MSSOPT;
		h->tcpopt[1] = MSS_LENGTH;
		hnputs(h->tcpmss, tcph->mss);
	}
	csum = ptclcsum(data, TCP_IPLEN, hdrlen+dlen+TCP_PHDRSIZE);
	hnputs(h->tcpcksum, csum);

	DBG(nhgetl(h->tcpdst))(Logtcpmsg, "%d > %d s %l8.8ux a %8.8lux %s w %.4ux l %d\n",
		tcph->source, tcph->dest,
		tcph->seq, tcph->ack, tcpflag((hdrlen<<10)|tcph->flags),
		tcph->wnd, dlen);

	return data;
}

int
ntohtcp(Tcp *tcph, Block **bpp)
{
	Tcphdr *h;
	byte *optr;
	ushort hdrlen;
	ushort i, optlen;

	*bpp = pullupblock(*bpp, TCP_PKT+TCP_HDRSIZE);
	if(*bpp == nil)
		return -1;

	h = (Tcphdr *)((*bpp)->rp);
	tcph->source = nhgets(h->tcpsport);
	tcph->dest = nhgets(h->tcpdport);
	tcph->seq = nhgetl(h->tcpseq);
	tcph->ack = nhgetl(h->tcpack);

	hdrlen = (h->tcpflag[0] & 0xf0)>>2;
	if(hdrlen < TCP_HDRSIZE) {
		freeblist(*bpp);
		return -1;
	}

	tcph->flags = h->tcpflag[1];
	tcph->wnd = nhgets(h->tcpwin);
	tcph->urg = nhgets(h->tcpurg);
	tcph->mss = 0;

	*bpp = pullupblock(*bpp, hdrlen+TCP_PKT);
	if(*bpp == nil)
		return -1;

	DBG(nhgetl(h->tcpsrc))(Logtcpmsg, "%d > %d s %l8.8ux a %8.8lux %s w %.4ux l %d\n",
		tcph->source, tcph->dest,
		tcph->seq, tcph->ack, tcpflag((hdrlen<<10)|tcph->flags),
		tcph->wnd, nhgets(h->length)-hdrlen-TCP_PKT);

	optr = h->tcpopt;
	for(i = TCP_HDRSIZE; i < hdrlen;) {
		switch(*optr++) {
		case EOLOPT:
			return hdrlen;
		case NOOPOPT:
			i++;
			break;
		case MSSOPT:
			optlen = *optr++;
			if(optlen == MSS_LENGTH)
				tcph->mss = nhgets(optr);
			i += optlen;
			break;
		}
	}
	return hdrlen;
}

/* Generate an initial sequence number and put a SYN on the send queue */
void
tcpsndsyn(Tcpctl *tcb)
{
	tcb->iss = (nrand(1<<16)<<16)|nrand(1<<16);
	tcb->rttseq = tcb->iss;
	tcb->snd.wl2 = tcb->iss;
	tcb->snd.una = tcb->iss;
	tcb->snd.ptr = tcb->rttseq;
	tcb->snd.nxt = tcb->rttseq;
	tcb->sndcnt++;
	tcb->flags |= FORCE;
	tcb->sndsyntime = msec;
}

void
sndrst(Ipaddr source, Ipaddr dest, ushort length, Tcp *seg)
{
	ushort tmp;
	Tcphdr ph;
	Block *hbp;
	byte rflags;

	if(seg->flags & RST)
		return;


	hnputl(ph.tcpsrc, dest);
	hnputl(ph.tcpdst, source);
	ph.proto = IP_TCPPROTO;
	hnputs(ph.tcplen, TCP_HDRSIZE);

	/* Swap port numbers */
	tmp = seg->dest;
	seg->dest = seg->source;
	seg->source = tmp;

	rflags = RST;

	/* convince the other end that this reset is in band */
	if(seg->flags & ACK) {
		seg->seq = seg->ack;
		seg->ack = 0;
	}
	else {
		rflags |= ACK;
		seg->ack = seg->seq;
		seg->seq = 0;
		if(seg->flags & SYN)
			seg->ack++;
		seg->ack += length;
		if(seg->flags & FIN)
			seg->ack++;
	}
	seg->flags = rflags;
	seg->wnd = 0;
	seg->urg = 0;
	seg->mss = 0;
	hbp = htontcp(seg, nil, &ph);
	if(hbp == nil)
		return;

	ipoput(hbp, 0, MAXTTL);
}

/*
 *  flush an incoming call; send a reset to the remote side and close the
 *  conversation
 */
void
tcpflushincoming(Conv *s)
{
	Tcp seg;
	byte dst[4];
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;

	seg.source = s->rport;
	seg.dest = s->lport;
	seg.flags = ACK;
	seg.seq = tcb->snd.ptr;
	tcb->last_ack = tcb->rcv.nxt;
	seg.ack = tcb->rcv.nxt;

	if(s->laddr == 0) {
		hnputl(dst, s->raddr);
		s->laddr = Mediagetsrc(dst);
	}
	sndrst(s->raddr, s->laddr, 0, &seg);
	localclose(s, nil);
}

/*
 *  send a reset to the remote side and close the conversation
 */
char*
tcphangup(Conv *s)
{
	Tcp seg;
	byte dst[4];
	Tcpctl *tcb;
	Tcphdr ph;
	Block *hbp;

	tcb = (Tcpctl*)s->ptcl;
	if(waserror()){
		qunlock(tcb);
		return up->error;
	}
	qlock(tcb);
	if(s->raddr != 0) {
		seg.source = s->lport;
		seg.dest = s->rport;
		seg.flags = RST | ACK;
		seg.ack = tcb->rcv.nxt;
		seg.seq = tcb->snd.ptr;
		seg.wnd = 0;
		seg.urg = 0;
		seg.mss = 0;
		tcb->last_ack = tcb->rcv.nxt;
		if(s->laddr == 0) {
			hnputl(dst, s->raddr);
			s->laddr = Mediagetsrc(dst);
		}
		hnputl(ph.tcpsrc, s->laddr);
		hnputl(ph.tcpdst, s->raddr);
		ph.proto = IP_TCPPROTO;
		hnputs(ph.tcplen, TCP_HDRSIZE);
		hbp = htontcp(&seg, nil, &ph);
		ipoput(hbp, 0, s->ttl);
	}
	localclose(s, nil);
	poperror();
	qunlock(tcb);
	return nil;
}

Conv*
tcpincoming(Conv *s, Tcp *segp, Ipaddr src, Ipaddr dst)
{
	Conv *new;
	Tcpctl *tcb;

	new = Fsnewcall(&fs, s, src, segp->source, dst, segp->dest);
	if(new == nil)
		return nil;

	memmove(new->ptcl, s->ptcl, sizeof(Tcpctl));
	tcb = (Tcpctl*)new->ptcl;
	tcb->flags &= ~CLONE;
	tcb->timer.arg = new;
	tcb->timer.state = TimerOFF;
	tcb->acktimer.arg = new;
	tcb->acktimer.state = TimerOFF;

	return new;
}

int
seq_within(ulong x, ulong low, ulong high)
{
	if(low <= high){
		if(low <= x && x <= high)
			return 1;
	}
	else {
		if(low >= x && x >= high)
			return 1;
	}
	return 0;
}

int
seq_lt(ulong x, ulong y)
{
	return x < y;
}

int
seq_le(ulong x, ulong y)
{
	return x <= y;
}

int
seq_gt(ulong x, ulong y)
{
	return x > y;
}

int
seq_ge(ulong x, ulong y)
{
	return x >= y;
}

/*
 *  use the time between the first SYN and it's ack as the
 *  initial round trip time
 */
void
tcpsynackrtt(Conv *s)
{
	Tcpctl *tcb;
	int delta;

	tcb = (Tcpctl*)s->ptcl;

	delta = msec - tcb->sndsyntime;
	tcb->srtt = delta<<LOGAGAIN;
	tcb->mdev = delta<<LOGDGAIN;

	/* halt round trip timer */
	tcphalt(&tcb->rtt_timer);
}

void
update(Conv *s, Tcp *seg)
{
	int rtt, delta;
	Tcpctl *tcb;
	ushort acked, expand;

	tcb = (Tcpctl*)s->ptcl;

	tcb->kacounter = MAXBACKOFF;	/* keep alive count down */

	if(seq_gt(seg->ack, tcb->snd.nxt)) {
		tcb->flags |= FORCE;
		return;
	}

	if(seq_ge(seg->ack,tcb->snd.wl2))
	if(seq_gt(seg->seq,tcb->snd.wl1) || (seg->seq == tcb->snd.wl1)) {
		if(seg->wnd != 0 && tcb->snd.wnd == 0)
			tcb->snd.ptr = tcb->snd.una;

		tcb->snd.wnd = seg->wnd;
		tcb->snd.wl1 = seg->seq;
		tcb->snd.wl2 = seg->ack;
	}

	if(!seq_gt(seg->ack, tcb->snd.una))
		return;

	/* something new was acked in this packet */
	tcb->flags |= ACKED;

	/* Compute the new send window size */
	acked = seg->ack - tcb->snd.una;
	if(tcb->cwind < tcb->snd.wnd) {
		if(tcb->cwind < tcb->ssthresh) {
			expand = tcb->mss;
			if(acked < expand)
				expand = acked;
		}
		else
			expand = ((int)tcb->mss * tcb->mss) / tcb->cwind;

		if(tcb->cwind + expand < tcb->cwind)
			expand = 65535 - tcb->cwind;
		if(tcb->cwind + expand > tcb->snd.wnd)
			expand = tcb->snd.wnd - tcb->cwind;
		if(expand != 0)
			tcb->cwind += expand;
	}

	/* Adjust the timers according to the round trip time */
	if(tcb->rtt_timer.state == TimerON && seq_ge(seg->ack, tcb->rttseq)) {
		tcphalt(&tcb->rtt_timer);
		if((tcb->flags&RETRAN) == 0) {
			tcb->backoff = 0;
			rtt = tcb->rtt_timer.start - tcb->rtt_timer.count;
			if(rtt == 0)
				rtt = 1;	/* otherwise all close systems will rexmit in 0 time */
			rtt *= MSPTICK;
			if (tcb->srtt == 0) {
				tcb->srtt = rtt << LOGAGAIN;
				tcb->mdev = rtt << LOGDGAIN;
			} else {
				delta = rtt - (tcb->srtt>>LOGAGAIN);
				tcb->srtt += delta;
				if(tcb->srtt <= 0)
					tcb->srtt = 1;

				delta = abs(delta) - (tcb->mdev>>LOGDGAIN);
				tcb->mdev += delta;
				if(tcb->mdev <= 0)
					tcb->mdev = 1;
			}
		}
	}

	if((tcb->flags & SYNACK) == 0) {
		tcb->flags |= SYNACK;
		acked--;
		tcb->sndcnt--;
	}

	qdiscard(s->wq, acked);

	tcb->sndcnt -= acked;
	tcb->snd.una = seg->ack;
	if(seq_gt(seg->ack, tcb->snd.urg))
		tcb->snd.urg = seg->ack;

	tcphalt(&tcb->timer);
	if(tcb->snd.una != tcb->snd.nxt)
		tcpgo(&tcb->timer);

	if(seq_lt(tcb->snd.ptr, tcb->snd.una))
		tcb->snd.ptr = tcb->snd.una;

	tcb->flags &= ~RETRAN;
	tcb->backoff = 0;
}

void
tcpiput(Block *bp)
{
	Tcp seg;
	Tcphdr *h;
	int hdrlen;
	Tcpctl *tcb;
	ushort length;
	Ipaddr source, dest;
	Conv *spec, *gen, *s, **p;

	h = (Tcphdr*)(bp->rp);

	dest = nhgetl(h->tcpdst);
	source = nhgetl(h->tcpsrc);
	length = nhgets(h->length);

	h->Unused = 0;
	hnputs(h->tcplen, length-TCP_PKT);
	if(ptclcsum(bp, TCP_IPLEN, length-TCP_IPLEN)) {
		tcp.csumerr++;
		netlog(Logtcp, "bad tcp proto cksum\n");
		freeblist(bp);
		return;
	}

	hdrlen = ntohtcp(&seg, &bp);
	if(hdrlen < 0){
		tcp.hlenerr++;
		netlog(Logtcp, "bad tcp hdr len\n");
		return;
	}

	/* trim the packet to the size claimed by the datagram */
	length -= hdrlen+TCP_PKT;
	bp = trimblock(bp, hdrlen+TCP_PKT, length);
	if(bp == nil){
		tcp.lenerr++;
		netlog(Logtcp, "tcp len < 0 after trim\n");
		return;
	}


	/* Look for a connection. failing that look for a listener. */
	for(p = tcp.conv; *p; p++) {
		s = *p;
		if(s->rport == seg.source &&
		   s->lport == seg.dest && s->raddr == source)
			break;
	}
	s = *p;
	if(s){
		/* can't send packets to a listener */
		tcb = (Tcpctl*)s->ptcl;
		if(tcb->state == Listen){
			freeblist(bp);
			return;
		}
	}
	if(s == nil && (seg.flags & SYN)) {
		/*
		 *  dump packets with bogus flags
		 */
		if(seg.flags & RST){
			freeblist(bp);
			return;
		}
		if(seg.flags & ACK) {
			sndrst(source, dest, length, &seg);
			freeblist(bp);
			return;
		}

		/*
		 *  find a listener specific to this port (spec) or,
		 *  failing that, a general one (gen)
		 */
		gen = nil;
		spec = nil;
		for(p = tcp.conv; *p; p++) {
			s = *p;
			tcb = (Tcpctl*)s->ptcl;
			if((tcb->flags & CLONE) == 0)
				continue;
			if(tcb->state != Listen)
				continue;
			if(s->rport == 0 && s->raddr == 0) {
				if(s->lport == seg.dest){
					spec = s;
					break;
				}
				if(s->lport == 0)
					gen = s;
			}
		}
		s = nil;
		if(spec != nil)
			s = tcpincoming(spec, &seg, source, dest);
		else
		if(gen != nil)
			s = tcpincoming(gen, &seg, source, dest);
	}
	if(s == nil) {
		freeblist(bp);
		sndrst(source, dest, length, &seg);
		return;
	}

	/* The rest of the input state machine is run with the control block
	 * locked and implements the state machine directly out of the RFC.
	 * Out-of-band data is ignored - it was always a bad idea.
	 */
	tcb = (Tcpctl*)s->ptcl;
	qlock(tcb);

	switch(tcb->state) {
	case Closed:
		sndrst(source, dest, length, &seg);
		goto raise;
	case Listen:
		if(seg.flags & SYN) {
			procsyn(s, &seg);
			tcpsndsyn(tcb);
			tcpsetstate(s, Syn_received);
			if(length != 0 || (seg.flags & FIN))
				break;
		}
		goto raise;
	case Syn_sent:
		if(seg.flags & ACK) {
			if(!seq_within(seg.ack, tcb->iss+1, tcb->snd.nxt)) {
				sndrst(source, dest, length, &seg);
				goto raise;
			}
		}
		if(seg.flags & RST) {
			if(seg.flags & ACK)
				localclose(s, Econrefused);
			goto raise;
		}

		if(seg.flags & SYN) {
			procsyn(s, &seg);
			if(seg.flags & ACK){
				update(s, &seg);
				tcpsynackrtt(s);
				tcpsetstate(s, Established);
			}
			else
				tcpsetstate(s, Syn_received);

			if(length != 0 || (seg.flags & FIN))
				break;

			freeblist(bp);
			goto output;
		}
		else
			freeblist(bp);

		qunlock(tcb);
		return;
	case Syn_received:
		/* doesn't matter if it's the correct ack, we're just trying to set timing */
		if(seg.flags & ACK)
			tcpsynackrtt(s);
		break;
	}

	/* Cut the data to fit the receive window */
	if(tcptrim(tcb, &seg, &bp, &length) == -1) {
		netlog(Logtcp, "tcp len < 0, %lux\n", seg.seq);
		update(s, &seg);
		if(tcb->sndcnt == 0 && tcb->state == Closing) {
			tcpsetstate(s, Time_wait);
			tcb->timer.start = MSL2*(1000 / MSPTICK);
			tcpgo(&tcb->timer);
		}
		if(!(seg.flags & RST)) {
			tcb->flags |= FORCE;
			goto output;
		}
		qunlock(tcb);
		return;
	}

	/* Cannot accept so answer with a rst */
	if(length && tcb->state == Closed) {
		sndrst(source, dest, length, &seg);
		goto raise;
	}

	/* The segment is beyond the current receive pointer so
	 * queue the data in the resequence queue
	 */
	if(seg.seq != tcb->rcv.nxt)
	if(length != 0 || (seg.flags & (SYN|FIN))) {
		update(s, &seg);
		addreseq(tcb, &seg, bp, length);
		tcb->flags |= FORCE;
		goto output;
	}

	/*
	 *  keep looping till we've processed this packet plus any
	 *  adjacent packets in the resequence queue
	 */
	for(;;) {
		if(seg.flags & RST) {
			localclose(s, Econrefused);
			goto raise;
		}

		if((seg.flags&ACK) == 0)
			goto raise;

		switch(tcb->state) {
		case Syn_received:
			if(!seq_within(seg.ack, tcb->snd.una+1, tcb->snd.nxt)){
				sndrst(source, dest, length, &seg);
				goto raise;
			}
			update(s, &seg);
			tcpsetstate(s, Established);
		case Established:
		case Close_wait:
			update(s, &seg);
			break;
		case Finwait1:
			update(s, &seg);
			if(tcb->sndcnt == 0){
				tcb->f2counter = MAXBACKOFF;
				tcpsetstate(s, Finwait2);
				tcb->timer.start = MSL2 * (1000 / MSPTICK);
				tcpgo(&tcb->timer);
			}
			break;
		case Finwait2:
			update(s, &seg);
			break;
		case Closing:
			update(s, &seg);
			if(tcb->sndcnt == 0) {
				tcpsetstate(s, Time_wait);
				tcb->timer.start = MSL2*(1000 / MSPTICK);
				tcpgo(&tcb->timer);
			}
			break;
		case Last_ack:
			update(s, &seg);
			if(tcb->sndcnt == 0) {
				localclose(s, nil);
				goto raise;
			}
		case Time_wait:
			tcb->flags |= FORCE;
			if(tcb->timer.state != TimerON)
				tcpgo(&tcb->timer);
		}

		if((seg.flags&URG) && seg.urg) {
			if(seq_gt(seg.urg + seg.seq, tcb->rcv.urg)) {
				tcb->rcv.urg = seg.urg + seg.seq;
				pullblock(&bp, seg.urg);
			}
		}
		else
		if(seq_gt(tcb->rcv.nxt, tcb->rcv.urg))
			tcb->rcv.urg = tcb->rcv.nxt;

		if(length == 0) {
			if(bp != nil)
				freeblist(bp);
		}
		else {
			switch(tcb->state){
			default:
				/* Ignore segment text */
				if(bp != nil)
					freeblist(bp);
				break;

			case Syn_received:
			case Established:
			case Finwait1:
				/* If we still have some data place on
				 * receive queue
				 */
				if(bp) {
					qpass(s->rq, bp);
					bp = nil;
				}
				tcb->rcv.nxt += length;
				tcprcvwin(s);
				if(tcb->acktimer.state != TimerON)
					tcpgo(&tcb->acktimer);

				/* force an ack if there's a lot of unacked data */
				if(tcb->rcv.nxt-tcb->last_ack > (QMAX>>4))
					tcb->flags |= FORCE;

				break;
			case Finwait2:
				/* no process to read the data, send a reset */
				if(bp != nil)
					freeblist(bp);
				sndrst(source, dest, length, &seg);
				qunlock(tcb);
				return;
			}
		}

		if(seg.flags & FIN) {
			tcb->flags |= FORCE;

			switch(tcb->state) {
			case Syn_received:
			case Established:
				tcb->rcv.nxt++;
				tcpsetstate(s, Close_wait);
				break;
			case Finwait1:
				tcb->rcv.nxt++;
				if(tcb->sndcnt == 0) {
					tcpsetstate(s, Time_wait);
					tcb->timer.start = MSL2*(1000/MSPTICK);
					tcpgo(&tcb->timer);
				}
				else
					tcpsetstate(s, Closing);
				break;
			case Finwait2:
				tcb->rcv.nxt++;
				tcpsetstate(s, Time_wait);
				tcb->timer.start = MSL2 * (1000/MSPTICK);
				tcpgo(&tcb->timer);
				break;
			case Close_wait:
			case Closing:
			case Last_ack:
				break;
			case Time_wait:
				tcpgo(&tcb->timer);
				break;
			}
		}

		/*
		 *  get next adjacent segment from the resequence queue.
		 *  dump/trim any overlapping segments
		 */
		for(;;) {
			if(tcb->reseq == nil)
				goto output;

			if(seq_ge(tcb->rcv.nxt, tcb->reseq->seg.seq) == 0)
				goto output;

			getreseq(tcb, &seg, &bp, &length);

			if(tcptrim(tcb, &seg, &bp, &length) == 0)
				break;
		}
	}
output:
	tcpoutput(s);
	qunlock(tcb);
	return;
raise:
	qunlock(tcb);
	freeblist(bp);
	tcpkick(s, 0);
}

/*
 *  always enters and exits with the tcb locked
 */
void
tcpoutput(Conv *s)
{
	int x;
	Tcp seg;
	int msgs;
	Tcphdr ph;
	Tcpctl *tcb;
	Block *hbp, *bp;
	int sndcnt, n, first;
	ulong ssize, dsize, usable, sent;

	tcb = (Tcpctl*)s->ptcl;

	switch(tcb->state) {
	case Listen:
	case Closed:
	case Finwait2:
		return;
	}

	/* force an ack when a window has opened up */
	if(tcb->rcv.blocked && tcb->rcv.wnd > 0){
		tcb->rcv.blocked = 0;
		tcb->flags |= FORCE;
	}

	first = tcb->snd.ptr == tcb->snd.una;
	for(msgs = 0; msgs < 100; msgs++) {
		sndcnt = tcb->sndcnt;
		sent = tcb->snd.ptr - tcb->snd.una;

		/* Don't send anything else until our SYN has been acked */
		if(sent != 0 && (tcb->flags & (SYNACK|FORCE)) == 0)
			break;

		/* Compute usable segment based on offered window and limit
		 * window probes to one
		 */
		if(tcb->snd.wnd == 0){
			if(sent != 0) {
				if ((tcb->flags&FORCE) == 0)
					break;
				tcb->snd.ptr = tcb->snd.una;
			}
			usable = 1;
		}
		else {
			usable = tcb->cwind;
			if(tcb->snd.wnd < usable)
				usable = tcb->snd.wnd;
			usable -= sent;

			/*
			 *  hold small pieces in the hopes that more will come along.
			 *  this is pessimal in synchronous communications so go ahead
			 *  and send if:
			 *   - all previous xmits are acked
			 *   - we've forced to send anyways
			 *   - we've just gotten an ACK for a previous packet
			 */
			if(!first)
			if(!(tcb->flags&(FORCE|ACKED)))
			if((sndcnt-sent) < tcb->mss)
				usable = 0;
		}
		tcb->flags &= ~ACKED;

		ssize = sndcnt-sent;
		if(usable < ssize)
			ssize = usable;
		if(tcb->mss < ssize)
			ssize = tcb->mss;
		dsize = ssize;
		seg.urg = 0;

		if(ssize == 0)
		if((tcb->flags&FORCE) == 0)
			break;

		tcphalt(&tcb->acktimer);

		tcb->flags &= ~FORCE;
		tcprcvwin(s);

		/* By default we will generate an ack */
		seg.source = s->lport;
		seg.dest = s->rport;
		seg.flags = ACK;
		seg.mss = 0;

		switch(tcb->state){
		case Syn_sent:
			seg.flags = 0;
			/* No break */
		case Syn_received:
			if(tcb->snd.ptr == tcb->iss){
				seg.flags |= SYN;
				dsize--;
				seg.mss = tcpmtu(s);
			}
			break;
		}
		tcb->last_ack = tcb->rcv.nxt;
		seg.seq = tcb->snd.ptr;
		seg.ack = tcb->rcv.nxt;
		seg.wnd = tcb->rcv.wnd;

		/* Pull out data to send */
		bp = nil;
		if(dsize != 0) {
			bp = qcopy(s->wq, dsize, sent);
			if(BLEN(bp) != dsize) {
				seg.flags |= FIN;
				dsize--;
			}
			netlog(Logtcp, "qcopy: dlen %d blen %d sndcnt %d qlen %d sent %d rp[0] %d\n",
				dsize, BLEN(bp), sndcnt, qlen(s->wq), sent, bp->rp[0]);
		}

		if(sent+dsize == sndcnt)
			seg.flags |= PSH;

		/* keep track of balance of resent data */
		if(tcb->snd.ptr < tcb->snd.nxt) {
			n = tcb->snd.nxt - tcb->snd.ptr;
			if(ssize < n)
				n = ssize;
			tcb->resent += n;
		}

		tcb->snd.ptr += ssize;

		/* Pull up the send pointer so we can accept acks
		 * for this window
		 */
		if(seq_gt(tcb->snd.ptr,tcb->snd.nxt))
			tcb->snd.nxt = tcb->snd.ptr;

		/* Fill in fields of pseudo IP header */
		hnputl(ph.tcpdst, s->raddr);
		if(s->laddr == 0)
			s->laddr = Mediagetsrc(ph.tcpdst);

		hnputl(ph.tcpsrc, s->laddr);
		hnputs(ph.tcpsport, s->lport);
		hnputs(ph.tcpdport, s->rport);

		/* Build header, link data and compute cksum */
		hbp = htontcp(&seg, bp, &ph);
		if(hbp == nil) {
			freeblist(bp);
			return;
		}

		/* Start the transmission timers if there is new data and we
		 * expect acknowledges
		 */
		if(ssize != 0){
			x = backoff(tcb->backoff) *
			    (tcb->mdev + (tcb->srtt>>LOGAGAIN) + MSPTICK) / MSPTICK;
			if(x > (10000/MSPTICK))
				x = 10000/MSPTICK;
			tcb->timer.start = x;

			if(tcb->timer.state != TimerON)
				tcpgo(&tcb->timer);

			/* If round trip timer isn't running, start it */
			if(tcb->rtt_timer.state != TimerON) {
				tcpgo(&tcb->rtt_timer);
				tcb->rttseq = tcb->snd.ptr;
			}
		}

		ipoput(hbp, 0, s->ttl);
	}
}

/*
 *  the BSD convention (hack?) for keep alives.  resend last byte acked.
 */
void
tcpkeepalive(Conv *s)
{
	Tcp seg;
	Tcphdr ph;
	Tcpctl *tcb;
	Block *hbp,*dbp;

	tcb = (Tcpctl*)s->ptcl;


	dbp = nil;
	seg.urg = 0;
	seg.source = s->lport;
	seg.dest = s->rport;
	seg.flags = ACK|PSH;
	seg.mss = 0;
	seg.seq = tcb->snd.una-1;
	seg.ack = tcb->rcv.nxt;
	seg.wnd = tcb->rcv.wnd;
	tcb->last_ack = tcb->rcv.nxt;
	if(tcb->state == Finwait2){
		seg.flags |= FIN;
	} else {
		dbp = allocb(1);
		dbp->wp++;
	}

	/* Fill in fields of pseudo IP header */
	hnputl(ph.tcpdst, s->raddr);
	if(s->laddr == 0)
		s->laddr = Mediagetsrc(ph.tcpdst);
	hnputl(ph.tcpsrc, s->laddr);
	hnputs(ph.tcpsport, s->lport);
	hnputs(ph.tcpdport, s->rport);

	/* Build header, link data and compute cksum */
	hbp = htontcp(&seg, dbp, &ph);
	if(hbp == nil) {
		freeblist(dbp);
		return;
	}

	ipoput(hbp, 0, s->ttl);
}

void
tcprxmit(Conv *s)
{
	Tcpctl *tcb;

	tcb = (Tcpctl*)s->ptcl;


	qlock(tcb);
	tcb->flags |= RETRAN|FORCE;
	tcb->snd.ptr = tcb->snd.una;

	/* Pull window down to a single packet and halve the slow
	 * start threshold
	 */
	tcb->ssthresh = tcb->cwind / 2;
	tcb->ssthresh = tcb->ssthresh;
	if(tcb->mss > tcb->ssthresh)
		tcb->ssthresh = tcb->mss;

	tcb->cwind = tcb->mss;
	tcpoutput(s);

	tcp.rexmit++;

	qunlock(tcb);
}

void
tcptimeout(void *arg)
{
	Conv *s;
	Tcpctl *tcb;
	int maxback;

	s = (Conv*)arg;
	tcb = (Tcpctl*)s->ptcl;


	switch(tcb->state){
	default:
		tcb->backoff++;
		if(tcb->state == Syn_sent)
			maxback = (3*MAXBACKOFF)/4;
		else
			maxback = MAXBACKOFF;
		if(tcb->backoff >= maxback) {
			localclose(s, Etimedout);
			break;
		}
		tcprxmit(s);
		break;
	case Finwait2:
		if(--(tcb->f2counter) <= 0)
			localclose(s, Etimedout);
		else {
			qlock(tcb);
			tcpkeepalive(s);
			qunlock(tcb);
			tcpgo(&tcb->timer);
		}
		break;
	case Time_wait:
		localclose(s, nil);
		break;
	}
}

int
inwindow(Tcpctl *tcb, int seq)
{
	return seq_within(seq, tcb->rcv.nxt, tcb->rcv.nxt+tcb->rcv.wnd-1);
}

void
procsyn(Conv *s, Tcp *seg)
{
	Tcpctl *tcb;
	int mtu;

	tcb = (Tcpctl*)s->ptcl;
	tcb->flags |= FORCE;

	tcb->rcv.nxt = seg->seq + 1;
	tcb->rcv.urg = tcb->rcv.nxt;
	tcb->snd.wl1 = seg->seq;
	tcb->irs = seg->seq;
	tcb->snd.wnd = seg->wnd;

	if(seg->mss != 0)
		tcb->mss = seg->mss;

	tcb->max_snd = seg->wnd;

	mtu = tcpmtu(s);
	if(tcb->mss > mtu)
		tcb->mss = mtu;
	tcb->cwind = tcb->mss;
}

void
addreseq(Tcpctl *tcb, Tcp *seg, Block *bp, ushort length)
{
	Reseq *rp, *rp1;

	rp = malloc(sizeof(Reseq));
	if(rp == nil){
		freeblist(bp);	/* bp always consumed by add_reseq */
		return;
	}

	rp->seg = *seg;
	rp->bp = bp;
	rp->length = length;

	/* Place on reassembly list sorting by starting seq number */
	rp1 = tcb->reseq;
	if(rp1 == nil || seq_lt(seg->seq, rp1->seg.seq)) {
		rp->next = rp1;
		tcb->reseq = rp;
		return;
	}

	for(;;) {
		if(rp1->next == nil || seq_lt(seg->seq, rp1->next->seg.seq)) {
			rp->next = rp1->next;
			rp1->next = rp;
			break;
		}
		rp1 = rp1->next;
	}
}

void
getreseq(Tcpctl *tcb, Tcp *seg, Block **bp, ushort *length)
{
	Reseq *rp;

	rp = tcb->reseq;
	if(rp == nil)
		return;

	tcb->reseq = rp->next;

	*seg = rp->seg;
	*bp = rp->bp;
	*length = rp->length;

	free(rp);
}

int
tcptrim(Tcpctl *tcb, Tcp *seg, Block **bp, ushort *length)
{
	ushort len;
	Block *nbp;
	byte accept;
	int dupcnt, excess;

	accept = 0;
	len = *length;
	if(seg->flags & SYN)
		len++;
	if(seg->flags & FIN)
		len++;

	if(tcb->rcv.wnd == 0) {
		if(len == 0 && seg->seq == tcb->rcv.nxt)
			return 0;
	}
	else {
		/* Some part of the segment should be in the window */
		if(inwindow(tcb,seg->seq))
			accept++;
		else
		if(len != 0) {
			if(inwindow(tcb, seg->seq+len-1) ||
			seq_within(tcb->rcv.nxt, seg->seq,seg->seq+len-1))
				accept++;
		}
	}
	if(!accept) {
		freeblist(*bp);
		return -1;
	}
	dupcnt = tcb->rcv.nxt - seg->seq;
	if(dupcnt > 0){
		tcb->rerecv += dupcnt;
		if(seg->flags & SYN){
			seg->flags &= ~SYN;
			seg->seq++;

			if (seg->urg > 1)
				seg->urg--;
			else
				seg->flags &= ~URG;
			dupcnt--;
		}
		if(dupcnt > 0){
			pullblock(bp, (ushort)dupcnt);
			seg->seq += dupcnt;
			*length -= dupcnt;

			if (seg->urg > dupcnt)
				seg->urg -= dupcnt;
			else {
				seg->flags &= ~URG;
				seg->urg = 0;
			}
		}
	}
	excess = seg->seq + *length - (tcb->rcv.nxt + tcb->rcv.wnd);
	if(excess > 0) {
		tcb->rerecv += excess;
		*length -= excess;
		nbp = copyblock(*bp, *length);
		freeblist(*bp);
		*bp = nbp;
		seg->flags &= ~FIN;
	}
	return 0;
}

void
tcpadvise(Block *bp, char *msg)
{
	Tcphdr *h;
	Tcpctl *tcb;
	Ipaddr source, dest;
	ushort psource, pdest;
	Conv *s, **p;

	h = (Tcphdr*)(bp->rp);

	dest = nhgetl(h->tcpdst);
	source = nhgetl(h->tcpsrc);
	psource = nhgets(h->tcpsport);
	pdest = nhgets(h->tcpdport);

	/* Look for a connection */
	for(p = tcp.conv; *p; p++) {
		s = *p;
		if(s->rport == pdest && s->lport == psource)
		if(s->raddr == dest && s->laddr == source){
			tcb = (Tcpctl*)s->ptcl;
			qlock(tcb);
			switch(tcb->state){
			case Syn_sent:
				localclose(s, msg);
				break;
			}
			qunlock(tcb);
			break;
		}
	}
	freeblist(bp);
}

char*
tcpctl(Conv* c, char** f, int n)
{
	if(n == 1 && strcmp(f[0], "hangup") == 0)
		return tcphangup(c);
	return "unknown control request";
}

void
tcpinit(Fs *fs)
{
	tcp.name = "tcp";
	tcp.kick = tcpkick;
	tcp.connect = tcpconnect;
	tcp.announce = tcpannounce;
	tcp.ctl = tcpctl;
	tcp.state = tcpstate;
	tcp.create = tcpcreate;
	tcp.close = tcpclose;
	tcp.rcv = tcpiput;
	tcp.advise = tcpadvise;
	tcp.ipproto = IP_TCPPROTO;
	tcp.nc = Nchans;
	tcp.ptclsize = sizeof(Tcpctl);

	kproc("tcpack", tcpackproc, 0);

	Fsproto(fs, &tcp);
}
