#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

typedef struct Icmp {
	byte	vihl;		/* Version and header length */
	byte	tos;		/* Type of service */
	byte	length[2];	/* packet length */
	byte	id[2];		/* Identification */
	byte	frag[2];	/* Fragment information */
	byte	ttl;		/* Time to live */
	byte	proto;		/* Protocol */
	byte	ipcksum[2];	/* Header checksum */
	byte	src[4];		/* Ip source */
	byte	dst[4];		/* Ip destination */
	byte	type;
	byte	code;
	byte	cksum[2];
	byte	icmpid[2];
	byte	seq[2];
	byte	data[1];
} Icmp;

enum {			/* Packet Types */
	EchoReply	= 0,
	Unreachable	= 3,
	SrcQuench	= 4,
	EchoRequest	= 8,
	TimeExceed	= 11,
	Timestamp	= 13,
	TimestampReply	= 14,
	InfoRequest	= 15,
	InfoReply	= 16,
};

enum {
	IP_ICMPPROTO	= 1,
	ICMP_IPSIZE	= 20,
	ICMP_HDRSIZE	= 8,
};

	Proto	icmp;
extern	Fs	fs;

static char*
icmpconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	Fsconnected(&fs, c, e);

	return e;
}

static int
icmpstate(char **s, Conv *c)
{
	USED(c);
	*s = "Datagram";
	return 1;
}

static void
icmpcreate(Conv *c)
{
	c->rq = qopen(64*1024, 0, 0, c);
	c->wq = qopen(64*1024, 0, 0, 0);
}

static char*
icmpannounce(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil);
		return e;
	Fsconnected(&fs, c, nil);

	return nil;
}

static void
icmpclose(Conv *c)
{
	qclose(c->rq);
	qclose(c->wq);
	c->laddr = 0;
	c->lport = 0;
	unlock(c);
}

static void
icmpkick(Conv *c, int l)
{
	Icmp *p;
	Block *bp;

	USED(l);
	bp = qget(c->wq);
	if(bp == nil)
		return;

	if(blocklen(bp) < ICMP_IPSIZE + ICMP_HDRSIZE){
		freeblist(bp);
		return;
	}
	p = (Icmp *)(bp->rp);
	hnputl(p->dst, c->raddr);
	hnputl(p->src, c->laddr);
	p->proto = IP_ICMPPROTO;
	hnputs(p->icmpid, c->lport);
	memset(p->cksum, 0, sizeof(p->cksum));
	hnputs(p->cksum, ptclcsum(bp, ICMP_IPSIZE, blocklen(bp) - ICMP_IPSIZE));
	ipoput(bp, 0, c->ttl);
}

extern void
icmpnoconv(Block *bp)
{
	Block	*nbp;
	Icmp	*p, *np;

	p = (Icmp *)bp->rp;
	nbp = allocb(ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8);
	nbp->wp += ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8;
	np = (Icmp *)nbp->rp;
	memmove(np->dst, p->src, sizeof(np->dst));
	memmove(np->src, p->dst, sizeof(np->src));
	memmove(np->data, bp->rp, ICMP_IPSIZE + 8);
	np->type = Unreachable;
	np->code = 3;
	np->proto = IP_ICMPPROTO;
	hnputs(np->icmpid, 0);
	hnputs(np->seq, 0);
	memset(np->cksum, 0, sizeof(np->cksum));
	hnputs(np->cksum, ptclcsum(nbp, ICMP_IPSIZE, blocklen(nbp) - ICMP_IPSIZE));
	ipoput(nbp, 0, MAXTTL);
}

static void
goticmpkt(Block *bp)
{
	Conv	**c, *s;
	Icmp	*p;
	Ipaddr	dst;
	ushort	recid;

	p = (Icmp *) bp->rp;
	dst = nhgetl(p->src);
	recid = nhgets(p->icmpid);
netlog(Logicmp, "goticmpkt from %i to %d\n", dst, recid);

	for(c = icmp.conv; *c; c++) {
		s = *c;
netlog(Logicmp, "conv %i %d %i %d\n", s->laddr, s->lport, s->raddr, s->rport);
		if(s->lport == recid && s->raddr == dst){
			qpass(s->rq, bp);
			return;
		}
	}
	freeblist(bp);
}

static Block *
mkechoreply(Block *bp)
{
	Icmp	*q;
	byte	ip[4];

	q = (Icmp *)bp->rp;
	memmove(ip, q->src, sizeof(q->dst));
	memmove(q->src, q->dst, sizeof(q->src));
	memmove(q->dst, ip,  sizeof(q->dst));
	q->type = EchoReply;
	memset(q->cksum, 0, sizeof(q->cksum));
	hnputs(q->cksum, ptclcsum(bp, ICMP_IPSIZE, blocklen(bp) - ICMP_IPSIZE));
	return bp;
}

static char *unreachcode[] =
{
[0]	"net unreachable",
[1]	"host unreachable",
[2]	"protocol unreachable",
[3]	"port unreachable",
[4]	"fragmentation needed and DF set",
[5]	"source route failed",
};

static void
icmpiput(Media *m, Block *bp)
{
	int	n, iplen;
	Icmp	*p;
	Block	*r;
	Proto	*pr;
	char	*msg;
	char	m2[128];

	USED(m);
	p = (Icmp *)bp->rp;
	netlog(Logicmp, "icmpiput %d %d\n", p->type, p->code);
	n = blocklen(bp);
	if(n < ICMP_IPSIZE+ICMP_HDRSIZE){
		icmp.hlenerr++;
		goto raise;
	}
	iplen = nhgets(p->length);
	if(iplen > n || (iplen % 1)){
		icmp.lenerr++;
		goto raise;
	}
	if(ptclcsum(bp, ICMP_IPSIZE, iplen - ICMP_IPSIZE)){
		icmp.csumerr++;
		goto raise;
	}
	switch(p->type) {
	case EchoRequest:
		r = mkechoreply(bp);
		ipoput(r, 0, MAXTTL);
		break;
	case Unreachable:
		if(p->code > 5 || p->code < 0)
			msg = unreachcode[1];
		else
			msg = unreachcode[p->code];

		bp->rp += ICMP_IPSIZE+ICMP_HDRSIZE;
		if(blocklen(bp) < 8){
			icmp.lenerr++;
			goto raise;
		}
		p = (Icmp *)bp->rp;
		pr = Fsrcvpcol(&fs, p->proto);
		if(pr != nil && pr->advise != nil) {
			(*pr->advise)(bp, msg);
			return;
		}

		bp->rp -= ICMP_IPSIZE+ICMP_HDRSIZE;
		goticmpkt(bp);
		break;
	case TimeExceed:
		if(p->code == 0){
			sprint(m2, "ttl exceeded at %I", p->src);

			bp->rp += ICMP_IPSIZE+ICMP_HDRSIZE;
			if(blocklen(bp) < 8){
				icmp.lenerr++;
				goto raise;
			}
			p = (Icmp *)bp->rp;
			pr = Fsrcvpcol(&fs, p->proto);
			if(pr != nil && pr->advise != nil) {
				(*pr->advise)(bp, m2);
				return;
			}
		}

		goticmpkt(bp);
		break;
	default:
		goticmpkt(bp);
		break;
	}
	return;

raise:
	freeblist(bp);
}

void
icmpadvise(Block *bp, char *msg)
{
	Conv	**c, *s;
	Icmp	*p;
	Ipaddr	dst;
	ushort	recid;

	p = (Icmp *) bp->rp;
	dst = nhgetl(p->dst);
	recid = nhgets(p->icmpid);

	for(c = icmp.conv; *c; c++) {
		s = *c;
		if(s->lport == recid && s->raddr == dst){
			qhangup(s->rq, msg);
			qhangup(s->wq, msg);
			break;
		}
	}
	freeblist(bp);
}

void
icmpinit(Fs *fs)
{
	icmp.name = "icmp";
	icmp.kick = icmpkick;
	icmp.connect = icmpconnect;
	icmp.announce = icmpannounce;
	icmp.state = icmpstate;
	icmp.create = icmpcreate;
	icmp.close = icmpclose;
	icmp.rcv = icmpiput;
	icmp.ctl = nil;
	icmp.advise = icmpadvise;
	icmp.ipproto = IP_ICMPPROTO;
	icmp.nc = 16;
	icmp.ptclsize = 0;

	Fsproto(fs, &icmp);
}
