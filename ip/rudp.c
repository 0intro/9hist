
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

#define DPRINT if(1)print

enum
{
	RUDP_PHDRSIZE	= 12,
	RUDP_HDRSIZE	= 36,
	RUDP_RHDRSIZE	= 16,
	RUDP_IPHDR	= 8,
	IP_RUDPPROTO	= 254,
	RUDP_USEAD6	= 36,
	RUDP_USEAD4	= 12,

	Rudprxms	= 200,
	Rudptickms	= 100,
	Rudpmaxxmit	= 1,

};

/*
 *  reliable header
 */
typedef struct Relhdr Relhdr;
struct Relhdr
{
	uchar	relseq[4];	/* id of this packet (or 0) */
	uchar	relsgen[4];	/* generation/time stamp */
	uchar	relack[4];	/* packet being acked (or 0) */
	uchar	relagen[4];	/* generation/time stamp */
};

typedef struct Rudphdr Rudphdr;
struct Rudphdr
{
	/* ip header */
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	Unused;	
	uchar	rudpproto;	/* Protocol */
	uchar	rudpplen[2];	/* Header plus data length */
	uchar	rudpsrc[4];	/* Ip source */
	uchar	rudpdst[4];	/* Ip destination */

	/* rudp header */
	uchar	rudpsport[2];	/* Source port */
	uchar	rudpdport[2];	/* Destination port */
	Relhdr	rhdr;		/* reliable header */
	uchar	rudplen[2];	/* data length */
	uchar	rudpcksum[2];	/* Checksum */
};


/*
 *  one state structure per destination
 */
typedef struct Reliable Reliable;
struct Reliable
{
	Reliable *next;

	uchar addr[IPaddrlen];	/* always V6 when put here */
	ushort	port;


	Block	*unacked;	/* unacked msg list */
	Block	*unackedtail;	/*  and its tail */

	int	timeout;	/* time since first unacked msg sent */
	int	xmits;		/* number of times first unacked msg sent */

	ulong	sndseq;		/* next packet to be sent */
	ulong	sndgen;		/*  and its generation */

	ulong	rcvseq;		/* last packet received */
	ulong	rcvgen;		/*  and its generation */

	ulong	acksent;	/* last ack sent */
	ulong	ackrcvd;	/* last msg for which ack was rcvd */
};



/* MIB II counters */
typedef struct Rudpstats Rudpstats;
struct Rudpstats
{
	ulong	rudpInDatagrams;
	ulong	rudpNoPorts;
	ulong	rudpInErrors;
	ulong	rudpOutDatagrams;
};

typedef struct Rudppriv Rudppriv;
struct Rudppriv
{


	/* MIB counters */
	Rudpstats	ustats;

	/* non-MIB stats */
	ulong		csumerr;		/* checksum errors */
	ulong		lenerr;			/* short packet */

};


static ulong generation = 0;
static Rendez rend;
/*
 *  protocol specific part of Conv
 */
typedef struct Rudpcb Rudpcb;
struct Rudpcb
{
	QLock;
	uchar	headers;
	Reliable *r;
};

/*
 * local functions 
 */
void	relsendack( Conv *, Reliable * );
int	reliput( Conv *, Block *, uchar *, ushort );
Reliable *relstate( Rudpcb *, uchar *, ushort );
void	relackproc( void * );
void	relackq( Reliable *, Block * );
void	relhangup( Conv *, Reliable * );
void	relrexmit( Conv *, Reliable * );

static char*
rudpconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	Fsconnected(c, e);

	return e;
}


static int
rudpstate(Conv *c, char *state, int n)
{
	USED(c);
	return snprint(state, n, "%s", "Reliable UDP");
}

static char*
rudpannounce(Conv *c, char** argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil)
		return e;
	Fsconnected(c, nil);

	return nil;
}

static void
rudpcreate(Conv *c)
{
	c->rq = qopen(64*1024, 1, 0, 0);
	c->wq = qopen(64*1024, 0, 0, 0);
}

static void
rudpclose(Conv *c)
{
	Rudpcb *ucb;
	Reliable *r, *nr;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
	c->lport = 0;
	c->rport = 0;

	ucb = (Rudpcb*)c->ptcl;
	ucb->headers = 0;
	qlock( ucb );
	for( r = ucb->r; r; r = nr ){
		nr = r->next;
		relhangup( c, r );
		free( r );
	}
	ucb->r = 0;

	qunlock( ucb );

	unlock(c);
}

void
rudpkick(Conv *c, int)
{
	Rudphdr *uh;
	ushort rport;
	uchar laddr[IPaddrlen], raddr[IPaddrlen];
	Block *bp;
	Rudpcb *ucb;
	Relhdr *rh;
	Reliable *r;
	int dlen, ptcllen;
	Rudppriv *upriv;
	Fs *f;

	upriv = c->p->priv;
	f = c->p->f;

	netlog(c->p->f, Logrudp, "rudp: kick\n");
	bp = qget(c->wq);
	if(bp == nil)
		return;

	ucb = (Rudpcb*)c->ptcl;
	switch(ucb->headers) {
	case 6:
		/* get user specified addresses */
		bp = pullupblock(bp, RUDP_USEAD6);
		if(bp == nil)
			return;
		ipmove(raddr, bp->rp);
		bp->rp += IPaddrlen;
		ipmove(laddr, bp->rp);
		bp->rp += IPaddrlen;
		/* pick interface closest to dest */
		if(ipforme(f, laddr) != Runi)
			findlocalip(f, laddr, raddr);
		rport = nhgets(bp->rp);

		bp->rp += 4;			/* Igonore local port */
		break;
	case 4:
		bp = pullupblock(bp, RUDP_USEAD4);
		if(bp == nil)
			return;
		v4tov6(raddr, bp->rp);
		bp->rp += IPv4addrlen;
		v4tov6(laddr, bp->rp);
		bp->rp += IPv4addrlen;
		if(ipforme(f, laddr) != Runi)
			findlocalip(f, laddr, raddr);
		rport = nhgets(bp->rp);

		bp->rp += 4;			/* Igonore local port */
		break;
	default:
		rport = 0;

		break;
	}

	dlen = blocklen(bp);

	/* Make space to fit rudp & ip header */
	bp = padblock(bp, RUDP_IPHDR+RUDP_HDRSIZE);
	if(bp == nil)
		return;

	uh = (Rudphdr *)(bp->rp);

	rh = &(uh->rhdr);

	ptcllen = dlen + (RUDP_HDRSIZE-RUDP_PHDRSIZE);
	uh->Unused = 0;
	uh->rudpproto = IP_RUDPPROTO;
	uh->frag[0] = 0;
	uh->frag[1] = 0;
	hnputs(uh->rudpplen, ptcllen);
	switch(ucb->headers){
	case 4:
	case 6:
		v6tov4(uh->rudpdst, raddr);
		hnputs(uh->rudpdport, rport);
		v6tov4(uh->rudpsrc, laddr);
		break;
	default:
		v6tov4(uh->rudpdst, c->raddr);
		hnputs(uh->rudpdport, c->rport);
		if(ipcmp(c->laddr, IPnoaddr) == 0)
			findlocalip(f, c->laddr, c->raddr);
		v6tov4(uh->rudpsrc, c->laddr);
		break;
	}
	hnputs(uh->rudpsport, c->lport);
	hnputs(uh->rudplen, ptcllen);
	uh->rudpcksum[0] = 0;
	uh->rudpcksum[1] = 0;


	qlock( ucb );
	r = relstate( ucb, raddr, rport );
	r->sndseq++;
	hnputl( rh->relseq, r->sndseq );
	hnputl( rh->relsgen, r->sndgen );

	hnputl( rh->relack, r->rcvseq );  /* ACK last rcvd packet */
	hnputl( rh->relagen, r->rcvgen );

	if(r->rcvseq < r->acksent)
		r->acksent = r->rcvseq;

	hnputs(uh->rudpcksum, ptclcsum(bp, RUDP_IPHDR, dlen+RUDP_HDRSIZE));

	relackq( r, bp );
	qunlock( ucb );

	upriv->ustats.rudpOutDatagrams++;



	DPRINT( "sent: %d/%d, %d/%d\n", 
		r->sndseq, r->sndgen, r->rcvseq, r->rcvgen );

	ipoput(f, bp, 0, c->ttl);
}

void
rudpiput(Proto *rudp, uchar *ia, Block *bp)
{
	int len, olen, ottl;
	Rudphdr *uh;
	Conv *c, **p;
	Rudpcb *ucb;
	uchar raddr[IPaddrlen], laddr[IPaddrlen];
	ushort rport, lport;
	Rudppriv *upriv;
	Fs *f;

	upriv = rudp->priv;
	f = rudp->f;

	upriv->ustats.rudpInDatagrams++;

	uh = (Rudphdr*)(bp->rp);

	/* Put back pseudo header for checksum 
	 * (remember old values for icmpnoconv()) 
	 */
	ottl = uh->Unused;
	uh->Unused = 0;
	len = nhgets(uh->rudplen);
	olen = nhgets(uh->rudpplen);
	hnputs(uh->rudpplen, len);

	v4tov6(raddr, uh->rudpsrc);
	v4tov6(laddr, uh->rudpdst);
	lport = nhgets(uh->rudpdport);
	rport = nhgets(uh->rudpsport);



	if(nhgets(uh->rudpcksum)) {
		if(ptclcsum(bp, RUDP_IPHDR, len+RUDP_PHDRSIZE)) {
			upriv->ustats.rudpInErrors++;
			netlog(f, Logrudp, "rudp: checksum error %I\n", raddr);
			DPRINT("rudp: checksum error %I\n", raddr);
			freeblist(bp);
			return;
		}
	}

	/* Look for a conversation structure for this port */
	c = nil;
	for(p = rudp->conv; *p; p++) {
		c = *p;
		if(c->inuse == 0)
			continue;
		if(c->lport == lport && (c->rport == 0 || c->rport == rport))
			break;
	}

	if(*p == nil) {
		upriv->ustats.rudpNoPorts++;
		netlog(f, Logrudp, "rudp: no conv %I!%d -> %I!%d\n", raddr, rport,
			laddr, lport);
		DPRINT( "rudp: no conv %I!%d -> %I!%d\n", raddr, rport,
			laddr, lport);
		uh->Unused = ottl;
		hnputs(uh->rudpplen, olen);
		icmpnoconv(f, bp);
		freeblist(bp);
		return;
	}

	ucb = (Rudpcb*)c->ptcl;

	qlock( ucb );
	if( reliput( c, bp, raddr, rport ) < 0 ){
		qunlock( ucb );
		freeb( bp );
		return;
	}

	/*
	 * Trim the packet down to data size
	 */

	len -= (RUDP_HDRSIZE-RUDP_PHDRSIZE);
	bp = trimblock(bp, RUDP_IPHDR+RUDP_HDRSIZE, len);
	if(bp == nil){
		netlog(f, Logrudp, "rudp: len err %I.%d -> %I.%d\n", 
			raddr, rport, laddr, lport);
		DPRINT( "rudp: len err %I.%d -> %I.%d\n", 
			raddr, rport, laddr, lport);
		upriv->lenerr++;
		return;
	}

	netlog(f, Logrudpmsg, "rudp: %I.%d -> %I.%d l %d\n", 
		raddr, rport, laddr, lport, len);



	switch(ucb->headers){
	case 6:
		/* pass the src address */
		bp = padblock(bp, RUDP_USEAD6);
		ipmove(bp->rp, raddr);
		if(ipforme(f, laddr) == Runi)
			ipmove(bp->rp+IPaddrlen, laddr);
		else
			ipmove(bp->rp+IPaddrlen, ia);
		hnputs(bp->rp+2*IPaddrlen, rport);
		hnputs(bp->rp+2*IPaddrlen+2, lport);
		break;
	case 4:
		/* pass the src address */
		bp = padblock(bp, RUDP_USEAD4);
		v6tov4(bp->rp, raddr);
		if(ipforme(f, laddr) == Runi)
			v6tov4(bp->rp+IPv4addrlen, laddr);
		else
			v6tov4(bp->rp+IPv4addrlen, ia);
		hnputs(bp->rp + 2*IPv4addrlen, rport);
		hnputs(bp->rp + 2*IPv4addrlen + 2, lport);
		break;
	default:
		/* connection oriented rudp */
		if(c->raddr == 0){
			/* save the src address in the conversation */
		 	ipmove(c->raddr, raddr);
			c->rport = rport;

			/* reply with the same ip address (if not broadcast) */
			if(ipforme(f, laddr) == Runi)
				ipmove(c->laddr, laddr);
			else
				v4tov6(c->laddr, ia);
		}
		break;
	}
	if(bp->next)
		bp = concatblock(bp);

	if(qfull(c->rq)){
		netlog(f, Logrudp, "rudp: qfull %I.%d -> %I.%d\n", raddr, rport,
			laddr, lport);
		freeblist(bp);
	}
	else
		qpass(c->rq, bp);
	
	qunlock( ucb );
}

char*
rudpctl(Conv *c, char **f, int n)
{
	Rudpcb *ucb;

	ucb = (Rudpcb*)c->ptcl;
	if(n == 1){
		if(strcmp(f[0], "headers4") == 0){
			ucb->headers = 4;
			return nil;
		} else if(strcmp(f[0], "headers") == 0){
			ucb->headers = 6;
			return nil;
		}
	}
	return "unknown control request";
}

void
rudpadvise(Proto *rudp, Block *bp, char *msg)
{
	Rudphdr *h;
	uchar source[IPaddrlen], dest[IPaddrlen];
	ushort psource, pdest;
	Conv *s, **p;

	h = (Rudphdr*)(bp->rp);

	v4tov6(dest, h->rudpdst);
	v4tov6(source, h->rudpsrc);
	psource = nhgets(h->rudpsport);
	pdest = nhgets(h->rudpdport);

	/* Look for a connection */
	for(p = rudp->conv; *p; p++) {
		s = *p;
		if(s->rport == pdest)
		if(s->lport == psource)
		if(ipcmp(s->raddr, dest) == 0)
		if(ipcmp(s->laddr, source) == 0){
			qhangup(s->rq, msg);
			qhangup(s->wq, msg);
			break;
		}
	}
	freeblist(bp);
}

int
rudpstats(Proto *rudp, char *buf, int len)
{
	Rudppriv *upriv;

	upriv = rudp->priv;
	return snprint(buf, len, "%d %d %d %d\n",
		upriv->ustats.rudpInDatagrams,
		upriv->ustats.rudpNoPorts,
		upriv->ustats.rudpInErrors,
		upriv->ustats.rudpOutDatagrams);
}

void
rudpinit(Fs *fs)
{

	Proto *rudp;

	rudp = smalloc(sizeof(Proto));
	rudp->priv = smalloc(sizeof(Rudppriv));
	rudp->name = "rudp";
	rudp->kick = rudpkick;
	rudp->connect = rudpconnect;
	rudp->announce = rudpannounce;
	rudp->ctl = rudpctl;
	rudp->state = rudpstate;
	rudp->create = rudpcreate;
	rudp->close = rudpclose;
	rudp->rcv = rudpiput;
	rudp->advise = rudpadvise;
	rudp->stats = rudpstats;
	rudp->ipproto = IP_RUDPPROTO;
	rudp->nc = 16;
	rudp->ptclsize = sizeof(Rudpcb);

	Fsproto(fs, rudp);

	kproc( "relackproc", relackproc, rudp );
	
}

/*********************************************/
/* Here starts the reliable helper functions */
/*********************************************/
/*
 *  Enqueue a copy of an unacked block for possible retransmissions
 */
void
relackq(Reliable *r, Block *bp)
{
	Block *np;

	np = copyblock(bp, blocklen(bp));
	if(r->unacked)
		r->unackedtail->list = np;
	else {
		/* restart timer */
		r->timeout = 0;
		r->xmits = 1;
		r->unacked = np;
	}
	r->unackedtail = np;
	np->list = nil;
}

/*
 *  retransmit unacked blocks
 */
void
relackproc(void *a)
{
	Rudpcb *ucb;
	Proto *rudp;
	Reliable *r;
	Conv **s, *c;

	rudp = (Proto *)a;
loop:
	tsleep(&rend, return0, 0, Rudptickms);

	for(s = rudp->conv; *s; s++) {
		c = *s;
		ucb = (Rudpcb*)c->ptcl;
		qlock( ucb );

		for(r = ucb->r; r; r = r->next){

			if(r->unacked != nil){
				r->timeout += Rudptickms;
				if(r->timeout > Rudprxms*r->xmits)
					relrexmit(c, r);
			}
			if(r->acksent < r->rcvseq)
				relsendack(c, r);
		}
		qunlock( ucb );
	}
	goto loop;
}

/*
 *  get the state record for a conversation
 */
Reliable*
relstate(Rudpcb *ucb, uchar *addr, ushort port)
{
	Reliable *r, **l;


	l = &ucb->r;
	for(r = *l; r; r = *l){
		if( memcmp( addr, r->addr, IPaddrlen) == 0 && 
		    port == r->port)
			break;
		l = &r->next;
	}

	/* no state for this addr/port, create some */
	if(r == nil){
		DPRINT( "new state %d\n", generation );
		if(generation == 0)
			generation = TK2SEC(MACHP(0)->ticks);
		r = smalloc( sizeof( Reliable ) );
		*l = r;
		memmove( r->addr, addr, IPaddrlen);
		r->port = port;
		r->unacked = 0;
		r->sndgen = generation++;
		r->sndseq = 0;
		r->ackrcvd = 0;
		r->rcvgen = 0;
		r->rcvseq = 0;
		r->acksent = 0;
		r->xmits = 0;
		r->timeout = 0;
	}

	return r;
}


/* 
 *  process a rcvd reliable packet. return -1 if not to be passed to user process,
 *  0 therwise.
 */
int
reliput(Conv *c, Block *bp, uchar *addr, ushort port)
{
	Block *nbp;
	Rudpcb *ucb;
	Rudphdr *uh;
	Reliable *r;
	Relhdr *rh;
	ulong seq, ack, sgen, agen, ackreal;



	/* get fields */
	uh = (Rudphdr *)(bp->rp);
	rh = &(uh->rhdr);
	seq = nhgetl(rh->relseq);
	sgen = nhgetl(rh->relsgen);
	ack = nhgetl(rh->relack);
	agen = nhgetl(rh->relagen);



	ucb = (Rudpcb*)c->ptcl;
	r = relstate(ucb, addr, port);
	

	DPRINT("rcvd %d/%d, %d/%d, r->sndgen = %d, r->ackrcvd = %d\n", 
		seq, sgen, ack, agen, r->sndgen, r->ackrcvd);

	/* dequeue acked packets */
	if(ack && agen == r->sndgen){
		DPRINT( "Here\n" );
		ackreal = 0;
		while(r->unacked != nil && ack > r->ackrcvd){
			nbp = r->unacked;
			r->unacked = nbp->list;
			DPRINT("%d/%d acked\n", ack, agen);
			freeb(nbp);
			r->ackrcvd++;
			ackreal = 1;
		}

		/*
		 *  retransmit next packet if the acked packet
		 *  was transmitted more than once
		 */
		if(ackreal && r->unacked != nil){
			r->timeout = 0;
			if(r->xmits > 1){
				r->xmits = 1;
				relrexmit(c, r);
			}
		}
		
	}

	/* make sure we're not talking to a new remote side */
	if(r->rcvgen != sgen){
		if(seq != 1)
			return -1;


		/* new connection */
		if(r->rcvgen != 0){
			DPRINT("new con r->rcvgen = %d, sgen = %d\n", r->rcvgen, sgen);
			relhangup(c, r);
		}
		r->rcvgen = sgen;
	}

	/* no message */
	if(seq == 0)
		return -1;

	/* refuse out of order delivery */
	if(seq != r->rcvseq + 1){
		DPRINT("out of sequence %d not %d\n", seq, r->rcvseq + 1);
		return -1;
	}
	r->rcvseq = seq;

	return 0;
}

void
relsendack(Conv *c, Reliable *r)
{
	Rudphdr *uh;
	Block *bp;
	Relhdr *rh;
	int ptcllen;
	Fs *f;

	bp = allocb(RUDP_IPHDR + RUDP_HDRSIZE);
	if(bp == nil)
		return;
	bp->wp += RUDP_IPHDR + RUDP_HDRSIZE;
	f = c->p->f;
	uh = (Rudphdr *)(bp->rp);
	rh = &(uh->rhdr);

	ptcllen = (RUDP_HDRSIZE-RUDP_PHDRSIZE);
	uh->Unused = 0;
	uh->rudpproto = IP_RUDPPROTO;
	uh->frag[0] = 0;
	uh->frag[1] = 0;
	hnputs(uh->rudpplen, ptcllen);



	v6tov4( uh->rudpdst, r->addr );
	hnputs(uh->rudpdport, r->port);
	hnputs(uh->rudpsport, c->lport);
	if(ipcmp(c->laddr, IPnoaddr) == 0)
		findlocalip(f, c->laddr, c->raddr);
	v6tov4(uh->rudpsrc, c->laddr);
	hnputs(uh->rudplen, ptcllen);



	hnputl(rh->relsgen, r->sndgen);
	hnputl(rh->relseq, 0);
	hnputl(rh->relagen, r->rcvgen);
	hnputl(rh->relack, r->rcvseq);

	if(r->acksent < r->rcvseq)
		r->acksent = r->rcvseq;

	uh->rudpcksum[0] = 0;
	uh->rudpcksum[1] = 0;
	hnputs(uh->rudpcksum, ptclcsum(bp, RUDP_IPHDR, RUDP_HDRSIZE));

	DPRINT( "sendack: %d/%d, %d/%d\n", 0, r->sndgen, r->rcvseq, r->rcvgen );
	ipoput(f, bp, 0, c->ttl);
}

/*
 *  called with ucb locked (and c locked if user initiated close)
 */
void
relhangup( Conv *, Reliable *r )
{
	Block *bp;

	/*
	 *  dump any unacked outgoing messages
	 */
	for(bp = r->unacked; bp != nil; bp = r->unacked){
		r->unacked = bp->list;
		bp->list = nil;
		freeb(bp);
	}

	r->rcvgen = 0;
	r->rcvseq = 0;
	r->acksent = 0;
	r->sndgen = generation++;
	r->sndseq = 0;
	r->ackrcvd = 0;
	r->xmits = 0;
	r->timeout = 0;
}

/*
 *  called with ucb locked
 */
void
relrexmit(Conv *c, Reliable *r)
{
	Block *np;
	Fs *f;
	f = c->p->f;
	r->timeout = 0;
	if(r->xmits++ > Rudpmaxxmit){
		relhangup(c, r);
		return;
	}

	np = copyblock(r->unacked, blocklen(r->unacked));
	//DPRINT("rxmit r->ackrvcd+1 = %d\n", r->ackrcvd+1);
	ipoput(f, np, 0, c->ttl);
}
