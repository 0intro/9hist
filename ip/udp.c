#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

#define DPRINT if(0)print

enum
{
	UDP_PHDRSIZE	= 12,
	UDP_HDRSIZE	= 20,
	UDP_IPHDR	= 8,
	IP_UDPPROTO	= 17,
	UDP_USEAD	= 36,

	Udprxms		= 200,
	Udptickms	= 100,
	Udpmaxxmit	= 10,
};

typedef struct Udphdr Udphdr;
struct Udphdr
{
	/* ip header */
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	Unused;	
	uchar	udpproto;	/* Protocol */
	uchar	udpplen[2];	/* Header plus data length */
	uchar	udpsrc[4];	/* Ip source */
	uchar	udpdst[4];	/* Ip destination */

	/* udp header */
	uchar	udpsport[2];	/* Source port */
	uchar	udpdport[2];	/* Destination port */
	uchar	udplen[2];	/* data length */
	uchar	udpcksum[2];	/* Checksum */
};

/*
 *  protocol specific part of Conv
 */
typedef struct Udpcb Udpcb;
struct Udpcb
{
	QLock;
	uchar	headers;
};

	Proto	udp;
	int	udpsum;
	uint	generation;
extern	Fs	fs;
	int	udpdebug;

static char*
udpconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	Fsconnected(&fs, c, e);

	return e;
}

static int
udpstate(Conv *c, char *state, int n)
{
	USED(c);
	return snprint(state, n, "%s", "Datagram");
}

static char*
udpannounce(Conv *c, char** argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil)
		return e;
	Fsconnected(&fs, c, nil);

	return nil;
}

static void
udpcreate(Conv *c)
{
	c->rq = qopen(64*1024, 1, 0, 0);
	c->wq = qopen(64*1024, 0, 0, 0);
}

static void
udpclose(Conv *c)
{
	Udpcb *ucb;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
	c->lport = 0;
	c->rport = 0;

	ucb = (Udpcb*)c->ptcl;
	ucb->headers = 0;

	unlock(c);
}

void
udpkick(Conv *c, int)
{
	Udphdr *uh;
	ushort rport;
	uchar laddr[IPaddrlen], raddr[IPaddrlen];
	Block *bp;
	Udpcb *ucb;
	int dlen, ptcllen;

	netlog(Logudp, "udp: kick\n");
	bp = qget(c->wq);
	if(bp == nil)
		return;

	ucb = (Udpcb*)c->ptcl;
	if(ucb->headers) {
		/* get user specified addresses */
		bp = pullupblock(bp, UDP_USEAD);
		if(bp == nil)
			return;
		ipmove(raddr, bp->rp);
		bp->rp += IPaddrlen;
		ipmove(laddr, bp->rp);
		bp->rp += IPaddrlen;
		if(ipforme(laddr) != Runi)
			findlocalip(laddr, raddr);	/* pick interface closest to dest */
		rport = nhgets(bp->rp);
		bp->rp += 2;
		/* ignore local port number */
		bp->rp += 2;
	} else {
		rport = 0;
	}

	dlen = blocklen(bp);

	/* Make space to fit udp & ip header */
	bp = padblock(bp, UDP_IPHDR+UDP_HDRSIZE);
	if(bp == nil)
		return;

	uh = (Udphdr *)(bp->rp);

	ptcllen = dlen + (UDP_HDRSIZE-UDP_PHDRSIZE);
	uh->Unused = 0;
	uh->udpproto = IP_UDPPROTO;
	uh->frag[0] = 0;
	uh->frag[1] = 0;
	hnputs(uh->udpplen, ptcllen);
	if(ucb->headers) {
		v6tov4(uh->udpdst, raddr);
		hnputs(uh->udpdport, rport);
		v6tov4(uh->udpsrc, laddr);
	} else {
		v6tov4(uh->udpdst, c->raddr);
		hnputs(uh->udpdport, c->rport);
		if(ipcmp(c->laddr, IPnoaddr) == 0)
			findlocalip(c->laddr, c->raddr);	/* pick interface closest to dest */
		v6tov4(uh->udpsrc, c->laddr);
	}
	hnputs(uh->udpsport, c->lport);
	hnputs(uh->udplen, ptcllen);
	uh->udpcksum[0] = 0;
	uh->udpcksum[1] = 0;

	hnputs(uh->udpcksum, ptclcsum(bp, UDP_IPHDR, dlen+UDP_HDRSIZE));

	ipoput(bp, 0, c->ttl);
}

void
udpiput(uchar *ia, Block *bp)
{
	int len, olen, ottl;
	Udphdr *uh;
	Conv *c, **p;
	Udpcb *ucb;
	uchar raddr[IPaddrlen], laddr[IPaddrlen];
	ushort rport, lport;

	uh = (Udphdr*)(bp->rp);

	/* Put back pseudo header for checksum (remember old values for icmpnoconv()) */
	ottl = uh->Unused;
	uh->Unused = 0;
	len = nhgets(uh->udplen);
	olen = nhgets(uh->udpplen);
	hnputs(uh->udpplen, len);

	v4tov6(raddr, uh->udpsrc);
	v4tov6(laddr, uh->udpdst);
	lport = nhgets(uh->udpdport);
	rport = nhgets(uh->udpsport);

	if(udpsum && nhgets(uh->udpcksum)) {
		if(ptclcsum(bp, UDP_IPHDR, len+UDP_PHDRSIZE)) {
			udp.csumerr++;
			netlog(Logudp, "udp: checksum error %I\n", raddr);
			freeblist(bp);
			return;
		}
	}

	/* Look for a conversation structure for this port */
	c = nil;
	for(p = udp.conv; *p; p++) {
		c = *p;
		if(c->inuse == 0)
			continue;
		if(c->lport == lport && (c->rport == 0 || c->rport == rport))
			break;
	}

	if(*p == nil) {
		netlog(Logudp, "udp: no conv %I.%d -> %I.%d\n", raddr, rport,
			laddr, lport);
		/* don't complain about broadcasts... */
		if(ipforme(raddr) == 0){
			uh->Unused = ottl;
			hnputs(uh->udpplen, olen);
			icmpnoconv(bp);
		}
		freeblist(bp);
		return;
	}

	/*
	 * Trim the packet down to data size
	 */
	len -= (UDP_HDRSIZE-UDP_PHDRSIZE);
	bp = trimblock(bp, UDP_IPHDR+UDP_HDRSIZE, len);
	if(bp == nil){
		netlog(Logudp, "udp: len err %I.%d -> %I.%d\n", raddr, rport,
			laddr, lport);
		udp.lenerr++;
		return;
	}

	netlog(Logudpmsg, "udp: %I.%d -> %I.%d l %d\n", raddr, rport,
		laddr, lport, len);

	ucb = (Udpcb*)c->ptcl;

	if(ucb->headers) {
		/* pass the src address */
		bp = padblock(bp, UDP_USEAD);
		ipmove(bp->rp, raddr);
		if(ipforme(laddr) == Runi)
			ipmove(bp->rp+IPaddrlen, laddr);
		else
			ipmove(bp->rp+IPaddrlen, ia);
		hnputs(bp->rp+2*IPaddrlen, rport);
		hnputs(bp->rp+2*IPaddrlen+2, lport);
	} else {
		/* connection oriented udp */
		if(c->raddr == 0){
			/* save the src address in the conversation */
		 	ipmove(c->raddr, raddr);
			c->rport = rport;

			/* reply with the same ip address (if not broadcast) */
			if(ipforme(laddr) == Runi)
				ipmove(c->laddr, laddr);
			else
				ipmove(c->laddr, ia);
		}
	}
	if(bp->next)
		bp = concatblock(bp);

	if(qfull(c->rq)){
		netlog(Logudp, "udp: qfull %I.%d -> %I.%d\n", raddr, rport,
			laddr, lport);
		freeblist(bp);
	}else
		qpass(c->rq, bp);
}

char*
udpctl(Conv *c, char **f, int n)
{
	Udpcb *ucb;

	ucb = (Udpcb*)c->ptcl;
	if(n == 1 && strcmp(f[0], "headers") == 0){
		ucb->headers = 1;
		return nil;
	}
	if(n == 1 && strcmp(f[0], "debug") == 0){
		udpdebug = 1;
		return nil;
	}
	return "unknown control request";
}

void
udpadvise(Block *bp, char *msg)
{
	Udphdr *h;
	uchar source[IPaddrlen], dest[IPaddrlen];
	ushort psource, pdest;
	Conv *s, **p;

	h = (Udphdr*)(bp->rp);

	v4tov6(dest, h->udpdst);
	v4tov6(source, h->udpsrc);
	psource = nhgets(h->udpsport);
	pdest = nhgets(h->udpdport);

	/* Look for a connection */
	for(p = udp.conv; *p; p++) {
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
udpstats(char *buf, int len)
{
	return snprint(buf, len,
		"udp: csum %d hlen %d len %d order %d rexmit %d\n",
		udp.csumerr, udp.hlenerr, udp.lenerr, udp.order, udp.rexmit);
}

void
udpinit(Fs *fs)
{
	udp.name = "udp";
	udp.kick = udpkick;
	udp.connect = udpconnect;
	udp.announce = udpannounce;
	udp.ctl = udpctl;
	udp.state = udpstate;
	udp.create = udpcreate;
	udp.close = udpclose;
	udp.rcv = udpiput;
	udp.advise = udpadvise;
	udp.stats = udpstats;
	udp.ipproto = IP_UDPPROTO;
	udp.nc = 16;
	udp.ptclsize = sizeof(Udpcb);

	Fsproto(fs, &udp);
}
