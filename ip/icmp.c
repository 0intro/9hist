#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

typedef struct Icmp {
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	ipcksum[2];	/* Header checksum */
	uchar	src[4];		/* Ip source */
	uchar	dst[4];		/* Ip destination */
	uchar	type;
	uchar	code;
	uchar	cksum[2];
	uchar	icmpid[2];
	uchar	seq[2];
	uchar	data[1];
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

	Maxtype		= 16,
};

enum {
	IP_ICMPPROTO	= 1,
	ICMP_IPSIZE	= 20,
	ICMP_HDRSIZE	= 8,
};

	Proto	icmp;
extern	Fs	fs;

static struct Icmpstats
{
	ulong	in[Maxtype+1];
	ulong	out[Maxtype+1];
} stats;

static char*
icmpconnect(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	Fsconnected(&fs, c, e);

	return e;
}

static int
icmpstate(Conv *c, char *state, int n)
{
	USED(c);
	return snprint(state, n, "%s", "Datagram");
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
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
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
	if(p->type <= Maxtype)
		stats.out[p->type]++;
	v6tov4(p->dst, c->raddr);
	v6tov4(p->src, c->laddr);
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
	netlog(Logicmp, "sending icmpnoconv -> %V\n", p->src);
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
	stats.out[Unreachable]++;
	ipoput(nbp, 0, MAXTTL);
}

static void
goticmpkt(Block *bp)
{
	Conv	**c, *s;
	Icmp	*p;
	uchar	dst[IPaddrlen];
	ushort	recid;

	p = (Icmp *) bp->rp;
	v4tov6(dst, p->src);
	recid = nhgets(p->icmpid);

	for(c = icmp.conv; *c; c++) {
		s = *c;
		if(s->lport == recid)
		if(ipcmp(s->raddr, dst) == 0){
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
	uchar	ip[4];

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
icmpiput(uchar*, Block *bp)
{
	int	n, iplen;
	Icmp	*p;
	Block	*r;
	Proto	*pr;
	char	*msg;
	char	m2[128];

	p = (Icmp *)bp->rp;
	netlog(Logicmp, "icmpiput %d %d\n", p->type, p->code);
	n = blocklen(bp);
	if(n < ICMP_IPSIZE+ICMP_HDRSIZE){
		icmp.hlenerr++;
		netlog(Logicmp, "icmp hlen %d\n", n);
		goto raise;
	}
	iplen = nhgets(p->length);
	if(iplen > n || (iplen % 1)){
		icmp.lenerr++;
		netlog(Logicmp, "icmp length %d\n", iplen);
		goto raise;
	}
	if(ptclcsum(bp, ICMP_IPSIZE, iplen - ICMP_IPSIZE)){
		icmp.csumerr++;
		netlog(Logicmp, "icmp checksum error\n");
		goto raise;
	}
	if(p->type <= Maxtype)
		stats.in[p->type]++;
	switch(p->type) {
	case EchoRequest:
		r = mkechoreply(bp);
		stats.out[EchoReply]++;
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
			sprint(m2, "ttl exceeded at %V", p->src);

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
	uchar	dst[IPaddrlen];
	ushort	recid;

	p = (Icmp *) bp->rp;
	v4tov6(dst, p->dst);
	recid = nhgets(p->icmpid);

	for(c = icmp.conv; *c; c++) {
		s = *c;
		if(s->lport == recid)
		if(ipcmp(s->raddr, dst) == 0){
			qhangup(s->rq, msg);
			qhangup(s->wq, msg);
			break;
		}
	}
	freeblist(bp);
}

int
icmpstats(char *buf, int len)
{
	int i, n;


	n = snprint(buf, len,
		"icmp: csum %d hlen %d len %d order %d rexmit %d\n",
		icmp.csumerr, icmp.hlenerr, icmp.lenerr, icmp.order, icmp.rexmit);
	n += snprint(buf+n, len-n, "\trcvd ");
	for(i = 0; i < Maxtype && len > n; i++)
		n += snprint(buf+n, len-n, " %d", stats.in[i]);	
	n += snprint(buf+n, len-n, "\n\tsent ");
	for(i = 0; i < Maxtype && len > n; i++)
		n += snprint(buf+n, len-n, " %d", stats.out[i]);
	if(n < len)
		n += snprint(buf+n, len-n, "\n");
	return n;	
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
	icmp.stats = icmpstats;
	icmp.ctl = nil;
	icmp.advise = icmpadvise;
	icmp.ipproto = IP_ICMPPROTO;
	icmp.nc = 16;
	icmp.ptclsize = 0;

	Fsproto(fs, &icmp);
}
