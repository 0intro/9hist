#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include 	"arp.h"
#include 	"ipdat.h"

#include	"devtab.h"

enum
{
	Nrprotocol	= 3,	/* Number of protocols supported by this driver */
	Nipsubdir	= 4,	/* Number of subdirectory entries per connection */
};

int 	udpsum = 1;
Queue	*Ipoutput;			/* Control message stream for tcp/il */
Ipifc	*ipifc;				/* IP protocol interfaces for stip */
Ipconv	*ipconv[Nrprotocol];		/* Connections for each protocol */
Network	*ipnet[Nrprotocol];		/* User level interface for protocol */
QLock	ipalloc;			/* Protocol port allocation lock */

/* ARPA User Datagram Protocol */
void	udpstiput(Queue *, Block *);
void	udpstoput(Queue *, Block *);
void	udpstopen(Queue *, Stream *);
void	udpstclose(Queue *);
/* ARPA Transmission Control Protocol */
void	tcpstiput(Queue *, Block *);
void	tcpstoput(Queue *, Block *);
void	tcpstopen(Queue *, Stream *);
void	tcpstclose(Queue *);
/* Plan9 Reliable Datagram Protocol */
void	iliput(Queue *, Block *);
void	iloput(Queue *, Block *);
void	ilopen(Queue *, Stream *);
void	ilclose(Queue *);

Qinfo tcpinfo = { tcpstiput, tcpstoput, tcpstopen, tcpstclose, "tcp", 0, 1 };
Qinfo udpinfo = { udpstiput, udpstoput, udpstopen, udpstclose, "udp" };
Qinfo ilinfo  = { iliput,    iloput,    ilopen,    ilclose,    "il"  };

Qinfo *protocols[] = { &tcpinfo, &udpinfo, &ilinfo, 0 };

void
ipinitnet(Network *np, Qinfo *stproto, Ipconv *cp)
{
	int j;

	for(j = 0; j < conf.ip; j++, cp++){
		cp->index = j;
		cp->stproto = stproto;
		cp->net = np;
	}
	np->name = stproto->name;
	np->nconv = conf.ip;
	np->devp = &ipinfo;
	np->protop = stproto;
	if(stproto != &udpinfo)
		np->listen = iplisten;
	np->clone = ipclonecon;
	np->prot = (Netprot *)ialloc(sizeof(Netprot) * conf.ip, 0);
	np->ninfo = 3;
	np->info[0].name = "remote";
	np->info[0].fill = ipremotefill;
	np->info[1].name = "local";
	np->info[1].fill = iplocalfill;
	np->info[2].name = "status";
	np->info[2].fill = ipstatusfill;
}

void
ipreset(void)
{
	int i, j;

	ipifc = (Ipifc *)ialloc(sizeof(Ipifc) * conf.ip, 0);

	for(i = 0; protocols[i]; i++) {
		ipconv[i] = (Ipconv *)ialloc(sizeof(Ipconv) * conf.ip, 0);
		ipnet[i] = (Network *)ialloc(sizeof(Network), 0);
		ipinitnet(ipnet[i], protocols[i], ipconv[i]);
		newqinfo(protocols[i]);
	}

	initfrag(conf.frag);
	tcpinit();
}

void
ipinit(void)
{
}

Chan *
ipattach(char *spec)
{
	Chan *c;
	int i;

	/* fail if ip is not yet configured */
	if(Ipoutput == 0)
		error(Enoproto);

	for(i = 0; protocols[i]; i++) {
		if(strcmp(spec, protocols[i]->name) == 0) {
			c = devattach('I', spec);
			c->dev = i;

			return (c);
		}
	}

	error(Enoproto);
}

Chan *
ipclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
ipwalk(Chan *c, char *name)
{
	return netwalk(c, name, ipnet[c->dev]);
}

void
ipstat(Chan *c, char *db)
{
	netstat(c, db, ipnet[c->dev]);
}

Chan *
ipopen(Chan *c, int omode)
{
	return netopen(c, omode, ipnet[c->dev]);
}

int
ipclonecon(Chan *c)
{
	Ipconv *new, *base;

	base = ipconv[c->dev];
	new = ipincoming(base, 0);
	if(new == 0)
		error(Enodev);
	return new - base;
}

Ipconv *
ipincoming(Ipconv *base, Ipconv *from)
{
	Ipconv *new, *etab;

	etab = &base[conf.ip];
	for(new = base; new < etab; new++) {
		if(new->ref == 0 && canqlock(new)) {
			if(new->ref || ipconbusy(new)) {
				qunlock(new);
				continue;
			}
			if(from)	/* copy ownership from listening channel */
				netown(new->net, new->index,
				       new->net->prot[from->index].owner, 0);
			else		/* current user becomes owner */
				netown(new->net, new->index, u->p->user, 0);

			new->ref = 1;
			new->newcon = 0;
			qunlock(new);
			return new;
		}	
	}
	return 0;
}

void
ipcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
ipremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
ipwstat(Chan *c, char *dp)
{
	netwstat(c, dp, ipnet[c->dev]);
}

void
ipclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long
ipread(Chan *c, void *a, long n, ulong offset)
{
	return netread(c, a, n, offset, ipnet[c->dev]);
}

long
ipwrite(Chan *c, char *a, long n, ulong offset)
{
	int 	m, backlog, type, priv;
	char 	*field[5], *ctlarg[5], buf[256];
	Port	port;
	Ipconv  *cp;

	type = STREAMTYPE(c->qid.path);
	if (type == Sdataqid)
		return streamwrite(c, a, n, 0); 

	if (type != Sctlqid)
		error(Eperm);

	cp = &ipconv[c->dev][STREAMID(c->qid.path)];

	m = n;
	if(m > sizeof(buf)-1)
		m = sizeof(buf)-1;
	strncpy(buf, a, m);
	buf[m] = '\0';

	m = getfields(buf, field, 5, ' ');
	if(m < 1)
		error(Ebadarg);

	if(strcmp(field[0], "connect") == 0) {
		if(ipconbusy(cp))
			error(Enetbusy);

		if(m != 2)
			error(Ebadarg);

		switch(getfields(field[1], ctlarg, 5, '!')) {
		default:
			error(Ebadarg);
		case 2:
			priv = 0;
			break;
		case 3:
			if(strcmp(ctlarg[2], "r") != 0)
				error(Eperm);
			priv = 1;
			break;
		}
		cp->dst = ipparse(ctlarg[0]);
		cp->pdst = atoi(ctlarg[1]);

		/* If we have no local port assign one */
		qlock(&ipalloc);
		if(cp->psrc == 0)
			cp->psrc = nextport(ipconv[c->dev], priv);
		qunlock(&ipalloc);

		if(cp->stproto == &tcpinfo)
			tcpstart(cp, TCP_ACTIVE, Streamhi, 0);
		else if(cp->stproto == &ilinfo)
			ilstart(cp, IL_ACTIVE, 20);

	}
	else if(strcmp(field[0], "disconnect") == 0) {
		if(cp->stproto != &udpinfo)
			error(Eperm);

		cp->dst = 0;
		cp->pdst = 0;
	}
	else if(strcmp(field[0], "announce") == 0) {
		if(ipconbusy(cp))
			error(Enetbusy);

		if(m != 2)
			error(Ebadarg);

		port = atoi(field[1]);

		qlock(&ipalloc);
		if(portused(ipconv[c->dev], port)) {
			qunlock(&ipalloc);	
			error(Einuse);
		}
		cp->psrc = port;
		qunlock(&ipalloc);

		if(cp->stproto == &tcpinfo)
			tcpstart(cp, TCP_PASSIVE, Streamhi, 0);
		else if(cp->stproto == &ilinfo)
			ilstart(cp, IL_PASSIVE, 10);

		if(cp->backlog == 0)
			cp->backlog = 3;
	}
	else if(strcmp(field[0], "backlog") == 0) {
		if(m != 2)
			error(Ebadarg);
		backlog = atoi(field[1]);
		if(backlog == 0)
			error(Ebadarg);
		if(backlog > 5)
			backlog = 5;
		cp->backlog = backlog;
	}
	else
		return streamwrite(c, a, n, 0);

	return n;
}

int
ipconbusy(Ipconv  *cp)
{
	if(cp->stproto == &tcpinfo)
	if(cp->tcpctl.state != Closed)
		return 1;

	if(cp->stproto == &ilinfo)
	if(cp->ilctl.state != Ilclosed)
		return 1;

	return 0;
}

void
udpstiput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

/*
 * udprcvmsg - called by stip to multiplex udp ports onto conversations
 */
void
udprcvmsg(Ipconv *muxed, Block *bp)
{
	Ipconv *ifc, *etab;
	Udphdr *uh;
	Port   dport, sport;
	ushort sum, len;
	Ipaddr addr;

	uh = (Udphdr *)(bp->rptr);

	/* Put back pseudo header for checksum */
	uh->Unused = 0;
	len = nhgets(uh->udplen);
	hnputs(uh->udpplen, len);

	addr = nhgetl(uh->udpsrc);

	if(udpsum && nhgets(uh->udpcksum)) {
		if(sum = ptcl_csum(bp, UDP_EHSIZE, len+UDP_PHDRSIZE)) {
			print("udp: checksum error %x (%d.%d.%d.%d)\n",
			      sum, fmtaddr(addr));
			
			freeb(bp);
			return;
		}
	}

	dport = nhgets(uh->udpdport);
	sport = nhgets(uh->udpsport);

	/* Look for a conversation structure for this port */
	etab = &muxed[conf.ip];
	for(ifc = muxed; ifc < etab; ifc++) {
		if(ifc->ref)
		if(ifc->psrc == dport)
		if(ifc->pdst == 0 || ifc->pdst == sport) {
			/* Trim the packet down to data size */
			len = len - (UDP_HDRSIZE-UDP_PHDRSIZE);
			bp = btrim(bp, UDP_EHSIZE+UDP_HDRSIZE, len);
			if(bp == 0)
				return;

			/* Stuff the src address into the remote file */
		 	ifc->dst = addr;
			ifc->pdst = sport;
			PUTNEXT(ifc->readq, bp);
			return;
		}
	}

	freeb(bp);
}

void
udpstoput(Queue *q, Block *bp)
{
	Ipconv *ipc;
	Udphdr *uh;
	int dlen, ptcllen, newlen;

	if(bp->type == M_CTL) {
		PUTNEXT(q, bp);
		return;
	}

	/* Prepend udp header to packet and pass on to ip layer */
	ipc = (Ipconv *)(q->ptr);
	if(ipc->psrc == 0)
		error(Enoport);

	if(bp->type != M_DATA) {
		freeb(bp);
		error(Ebadctl);
	}

	/* Only allow atomic udp writes to form datagrams */
	if(!(bp->flags & S_DELIM)) {
		freeb(bp);
		error(Emsgsize);
	}

	/* Round packet up to even number of bytes and check we can
	 * send it
	 */
	dlen = blen(bp);
	if(dlen > UDP_DATMAX) {
		freeb(bp);
		error(Emsgsize);
	}
	newlen = bround(bp, 1);

	/* Make space to fit udp & ip & ethernet header */
	bp = padb(bp, UDP_EHSIZE + UDP_HDRSIZE);

	uh = (Udphdr *)(bp->rptr);

	ptcllen = dlen + (UDP_HDRSIZE-UDP_PHDRSIZE);
	uh->Unused = 0;
	uh->udpproto = IP_UDPPROTO;
	uh->frag[0] = 0;
	uh->frag[1] = 0;
	hnputs(uh->udpplen, ptcllen);
	hnputl(uh->udpdst, ipc->dst);
	hnputl(uh->udpsrc, Myip[Myself]);
	hnputs(uh->udpsport, ipc->psrc);
	hnputs(uh->udpdport, ipc->pdst);
	hnputs(uh->udplen, ptcllen);
	uh->udpcksum[0] = 0;
	uh->udpcksum[1] = 0;

	hnputs(uh->udpcksum, ptcl_csum(bp, UDP_EHSIZE, newlen+UDP_HDRSIZE));
	PUTNEXT(q, bp);
}

void
udpstclose(Queue *q)
{
	Ipconv *ipc;

	ipc = (Ipconv *)(q->ptr);

	ipc->psrc = 0;
	ipc->pdst = 0;
	ipc->dst = 0;

	closeipifc(ipc->ipinterface);
}

void
udpstopen(Queue *q, Stream *s)
{
	Ipconv *ipc;

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_UDPPROTO, udprcvmsg, ipconv[s->dev],
			            1500, 512, ETHER_HDR, "UDP");

	ipc->readq = RD(q);	
	RD(q)->ptr = (void *)ipc;
	WR(q)->next->ptr = (void *)ipc->ipinterface;
	WR(q)->ptr = (void *)ipc;
}

void
tcpstiput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

void
tcpstoput(Queue *q, Block *bp)
{
	Ipconv *s;
	Tcpctl *tcb; 
	int len, errnum;
	Block *f;

	s = (Ipconv *)(q->ptr);
	tcb = &s->tcpctl;

	if(bp->type == M_CTL) {
		PUTNEXT(q, bp);
		return;
	}

	if(s->psrc == 0)
		error(Enoport);

	/* Report asynchronous errors */
	if(s->err)
		error(s->err);

	switch(tcb->state) {
	case Listen:
		tcb->flags |= ACTIVE;
		send_syn(tcb);
		setstate(s, Syn_sent);

		/* No break */
	case Syn_sent:
	case Syn_received:
	case Established:
	case Close_wait:
		qlock(tcb);
		if(waserror()) {
			qunlock(tcb);
			nexterror();
		}
		tcb->sndcnt += blen(bp);
		if(tcb->sndq == 0)
			tcb->sndq = bp;
		else {
			for(f = tcb->sndq; f->next; f = f->next)
				;
			f->next = bp;
		}
		tcprcvwin(s);
		tcp_output(s);
		poperror();
		qunlock(tcb);
		break;

	default:
		freeb(bp);
		error(Ehungup);
	}	
}

void
tcpstopen(Queue *q, Stream *s)
{
	Ipconv *ipc;
	static int tcpkprocs;

	if(!Ipoutput) {
		Ipoutput = WR(q);
		s->opens++;
		s->inuse++;
	}

	/* Flow control and tcp timer processes */
	if(tcpkprocs == 0) {
		tcpkprocs = 1;
		kproc("tcpack", tcpackproc, 0);
		kproc("tcpflow", tcpflow, &ipconv[s->dev]);

	}

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_TCPPROTO, tcp_input, ipconv[s->dev], 
			            1500, 512, ETHER_HDR, "TCP");

	ipc->readq = RD(q);
	ipc->readq->rp = &tcpflowr;
	ipc->err = 0;

	RD(q)->ptr = (void *)ipc;
	WR(q)->next->ptr = (void *)ipc->ipinterface;
	WR(q)->ptr = (void *)ipc;
}

void
ipremotefill(Chan *c, char *buf, int len)
{
	int connection;
	Ipconv *cp;

	connection = STREAMID(c->qid.path);
	cp = &ipconv[c->dev][connection];
	sprint(buf, "%d.%d.%d.%d %d\n", fmtaddr(cp->dst), cp->pdst);
}

void
iplocalfill(Chan *c, char *buf, int len)
{
	int connection;
	Ipconv *cp;

	connection = STREAMID(c->qid.path);
	cp = &ipconv[c->dev][connection];
	sprint(buf, "%d.%d.%d.%d %d\n", fmtaddr(Myip[Myself]), cp->psrc);
}

void
ipstatusfill(Chan *c, char *buf, int len)
{
	int connection;
	Ipconv *cp;

	connection = STREAMID(c->qid.path);
	cp = &ipconv[c->dev][connection];
	if(cp->stproto == &tcpinfo)
		sprint(buf, "tcp/%d %d %s %s\n", connection, cp->ref,
			tcpstate[cp->tcpctl.state],
			cp->tcpctl.flags & CLONE ? "listen" : "connect");
	else if(cp->stproto == &ilinfo)
		sprint(buf, "il/%d %d %s rtt %d ms %d csum\n", connection, cp->ref,
			ilstate[cp->ilctl.state], cp->ilctl.rtt,
			cp->ipinterface ? cp->ipinterface->chkerrs : 0);
	else
		sprint(buf, "%s/%d %d\n", cp->stproto->name, connection, cp->ref);
}

int
iphavecon(Ipconv *s)
{
	return s->curlog;
}

int
iplisten(Chan *c)
{
	Ipconv *etab, *new;
	Ipconv *s, *base;
	int connection;
	Ipconv *cp;

	connection = STREAMID(c->qid.path);
	s = &ipconv[c->dev][connection];
	base = ipconv[c->dev];

	if(s->stproto == &tcpinfo)
	if(s->tcpctl.state != Listen)
		error(Enolisten);

	if(s->stproto == &ilinfo)
	if(s->ilctl.state != Illistening)
		error(Enolisten);

	qlock(&s->listenq);
	if(waserror()) {
		qunlock(&s->listenq);
		nexterror();
	}

	for(;;) {
		sleep(&s->listenr, iphavecon, s);
		poperror();
		new = base;
 		for(etab = &base[conf.ip]; new < etab; new++) {
			if(new->newcon) {
				qlock(s);
				s->curlog--;
				qunlock(s);
				new->newcon = 0;
				qunlock(&s->listenq);
				return new - base;
			}
		}
		print("iplisten: no newcon\n");
	}
}

void
tcpstclose(Queue *q)
{
	Ipconv *s;
	Tcpctl *tcb;

	s = (Ipconv *)(q->ptr);
	tcb = &s->tcpctl;

	/* Not interested in data anymore */
	qlock(s);
	s->readq = 0;
	qunlock(s);

	switch(tcb->state){
	case Closed:
	case Listen:
	case Syn_sent:
		close_self(s, 0);
		break;

	case Syn_received:
	case Established:
		tcb->sndcnt++;
		tcb->snd.nxt++;
		setstate(s, Finwait1);
		goto output;

	case Close_wait:
		tcb->sndcnt++;
		tcb->snd.nxt++;
		setstate(s, Last_ack);
	output:
		qlock(tcb);
		if(waserror()) {
			qunlock(tcb);
			nexterror();
		}
		tcp_output(s);
		poperror();
		qunlock(tcb);
		break;
	}
}


static	short	endian	= 1;
static	char*	aendian	= (char*)&endian;
#define	LITTLE	*aendian

ushort
ptcl_bsum(uchar *addr, int len)
{
	ulong losum, hisum, mdsum, x;
	ulong t1, t2;

	losum = 0;
	hisum = 0;
	mdsum = 0;

	x = 0;
	if((ulong)addr & 1) {
		if(len) {
			hisum += addr[0];
			len--;
			addr++;
		}
		x = 1;
	}
	while(len >= 16) {
		t1 = *(ushort*)(addr+0);
		t2 = *(ushort*)(addr+2);	mdsum += t1;
		t1 = *(ushort*)(addr+4);	mdsum += t2;
		t2 = *(ushort*)(addr+6);	mdsum += t1;
		t1 = *(ushort*)(addr+8);	mdsum += t2;
		t2 = *(ushort*)(addr+10);	mdsum += t1;
		t1 = *(ushort*)(addr+12);	mdsum += t2;
		t2 = *(ushort*)(addr+14);	mdsum += t1;
		mdsum += t2;
		len -= 16;
		addr += 16;
	}
	while(len >= 2) {
		mdsum += *(ushort*)addr;
		len -= 2;
		addr += 2;
	}
	if(x) {
		if(len)
			losum += addr[0];
		if(LITTLE)
			losum += mdsum;
		else
			hisum += mdsum;
	} else {
		if(len)
			hisum += addr[0];
		if(LITTLE)
			hisum += mdsum;
		else
			losum += mdsum;
	}

	losum += hisum >> 8;
	losum += (hisum & 0xff) << 8;
	while(hisum = losum>>16)
		losum = hisum + (losum & 0xffff);

	return losum & 0xffff;
}

ushort
ptcl_csum(Block *bp, int offset, int len)
{
	uchar *addr;
	ulong losum, hisum;
	ushort csum;
	int odd, blen, x;

	/* Correct to front of data area */
	while(bp && offset && offset >= BLEN(bp)) {
		offset -= BLEN(bp);
		bp = bp->next;
	}
	if(bp == 0)
		return 0;

	addr = bp->rptr + offset;
	blen = BLEN(bp) - offset;

	if(bp->next == 0)
		return ~ptcl_bsum(addr, MIN(len, blen)) & 0xffff;

	losum = 0;
	hisum = 0;

	odd = 0;
	while(len) {
		x = MIN(len, blen);
		csum = ptcl_bsum(addr, x);
		if(odd)
			hisum += csum;
		else
			losum += csum;
		odd = (odd+x) & 1;
		len -= x;

		bp = bp->next;
		if(bp == 0)
			break;
		blen = BLEN(bp);
		addr = bp->rptr;
	}

	losum += hisum>>8;
	losum += (hisum&0xff)<<8;
	while((csum = losum>>16) != 0)
		losum = csum + (losum & 0xffff);

	return ~losum & 0xffff;
}

Block *
btrim(Block *bp, int offset, int len)
{
	Block *nb, *startb;
	ulong l;

	if(blen(bp) < offset+len) {
		freeb(bp);
		return 0;
	}

	while((l = BLEN(bp)) < offset) {
		offset -= l;
		nb = bp->next;
		bp->next = 0;
		freeb(bp);
		bp = nb;
	}

	startb = bp;
	bp->rptr += offset;

	while((l = BLEN(bp)) < len) {
		len -= l;
		bp = bp->next;
	}

	bp->wptr -= (BLEN(bp) - len);
	bp->flags |= S_DELIM;

	if(bp->next) {
		freeb(bp->next);
		bp->next = 0;
	}

	return(startb);
}

Ipconv *
portused(Ipconv *ic, Port port)
{
	Ipconv *ifc, *etab;

	if(port == 0)
		return 0;

	etab = &ic[conf.ip];
	for(ifc = ic; ifc < etab; ifc++)
		if(ifc->psrc == port) 
			return ifc;

	return 0;
}

static Port lastport[2] = { PORTALLOC-1, PRIVPORTALLOC-1 };

Port
nextport(Ipconv *ic, int priv)
{
	Port base;
	Port max;
	Port *p;
	Port i;

	if(priv){
		base = PRIVPORTALLOC;
		max = PORTALLOC;
		p = &lastport[1];
	} else {
		base = PORTALLOC;
		max = PORTMAX;
		p = &lastport[0];
	}
	
	for(i = *p + 1; i < max; i++)
		if(!portused(ic, i))
			return(*p = i);
	for(i = base ; i <= *p; i++)
		if(!portused(ic, i))
			return(*p = i);

	return(0);
}

/* NEEDS HASHING ! */

Ipconv *
ip_conn(Ipconv *ic, Port dst, Port src, Ipaddr dest, char proto)
{
	Ipconv *s, *etab;

	/* Look for a conversation structure for this port */
	etab = &ic[conf.ip];
	for(s = ic; s < etab; s++) {
		if(s->psrc == dst)
		if(s->pdst == src)
		if(s->dst == dest || dest == 0)
			return s;
	}

	return 0;
}
