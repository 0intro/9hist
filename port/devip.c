#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include 	"arp.h"
#include 	"ipdat.h"

#include	"devtab.h"

enum
{
	Nrprotocol	= 3,		/* Number of protocols supported by this driver */
	Nipsubdir	= 4,		/* Number of subdirectory entries per connection */
};

int 	udpsum = 1;
Queue	*Ipoutput;		/* Control message stream for tcp/il */
Ipifc	*ipifc;			/* IP protocol interfaces for stip */
Ipconv	*ipconv[Nrprotocol];	/* Connections for each protocol */
QLock	ipalloc;		/* Protocol port allocation lock */

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

Qinfo tcpinfo = { tcpstiput, tcpstoput, tcpstopen, tcpstclose, "tcp" };
Qinfo udpinfo = { udpstiput, udpstoput, udpstopen, udpstclose, "udp" };
Qinfo ilinfo  = { iliput,    iloput,    ilopen,    ilclose,    "il"  };

Qinfo *protocols[] = { &tcpinfo, &udpinfo, &ilinfo, 0 };

void
ipinitifc(Ipifc *ifc, Qinfo *stproto, Ipconv *cp)
{
	int j;

	for(j = 0; j < conf.ip; j++, cp++){
		cp->index = j;
		cp->stproto = stproto;
		cp->ipinterface = ifc;
	}
	ifc->net.name = stproto->name;
	ifc->net.nconv = conf.ip;
	ifc->net.devp = &ipinfo;
	ifc->net.protop = stproto;
	if(stproto != &udpinfo)
		ifc->net.listen = iplisten;
	ifc->net.clone = ipclonecon;
	ifc->net.prot = (Netprot *)ialloc(sizeof(Netprot) * conf.ip, 0);
	ifc->net.ninfo = 3;
	ifc->net.info[0].name = "remote";
	ifc->net.info[0].fill = ipremotefill;
	ifc->net.info[1].name = "local";
	ifc->net.info[1].fill = iplocalfill;
	ifc->net.info[2].name = "status";
	ifc->net.info[2].fill = ipstatusfill;
}

void
ipreset(void)
{
	int i, j;

	ipifc = (Ipifc *)ialloc(sizeof(Ipifc) * conf.ip, 0);

	for(i = 0; protocols[i]; i++) {
		ipconv[i] = (Ipconv *)ialloc(sizeof(Ipconv) * conf.ip, 0);
		ipinitifc(&ipifc[i], protocols[i], ipconv[i]);
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
	return netwalk(c, name, &ipifc[c->dev].net);
}

void
ipstat(Chan *c, char *db)
{
	netstat(c, db, &ipifc[c->dev].net);
}

Chan *
ipopen(Chan *c, int omode)
{
	return netopen(c, omode, &ipifc[c->dev].net);
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
	Ipifc *ifc;

	etab = &base[conf.ip];
	for(new = base; new < etab; new++) {
		if(new->ref == 0 && canqlock(new)) {
			if(new->ref ||
		          (new->stproto == &tcpinfo && new->tcpctl.state != CLOSED) ||
			  (new->stproto == &ilinfo && new->ilctl.state != Ilclosed)) {
				qunlock(new);
				continue;
			}
			ifc = base->ipinterface;
			if(from)
				/* copy ownership from listening channel */
				netown(&ifc->net, new->index, ifc->net.prot[from->index].owner, 0);
			else
				/* current user becomes owner */
				netown(&ifc->net, new->index, u->p->user, 0);
			new->ref = 1;
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
	netwstat(c, dp, &ipifc[c->dev].net);
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
	return netread(c, a, n, offset,  &ipifc[c->dev].net);
}

long
ipwrite(Chan *c, char *a, long n, ulong offset)
{
	int 	m, backlog, type;
	char 	*field[5], *ctlarg[5], buf[256];
	Port	port, base;
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
		errors("bad ip control");

	if(strcmp(field[0], "connect") == 0) {
		if((cp->stproto == &tcpinfo && cp->tcpctl.state != CLOSED) ||
		   (cp->stproto == &ilinfo && cp->ilctl.state != Ilclosed))
				error(Edevbusy);

		if(m != 2)
			error(Ebadarg);

		switch(getfields(field[1], ctlarg, 5, '!')) {
		default:
			error(Ebadarg);
		case 2:
			base = PORTALLOC;
			break;
		case 3:
			if(strcmp(ctlarg[2], "r") != 0)
				error(Eperm);
			base = PRIVPORTALLOC;
			break;
		}
		cp->dst = ipparse(ctlarg[0]);
		cp->pdst = atoi(ctlarg[1]);

		/* If we have no local port assign one */
		qlock(&ipalloc);
		if(cp->psrc == 0)
			cp->psrc = nextport(ipconv[c->dev], base);
		qunlock(&ipalloc);

		if(cp->stproto == &tcpinfo)
			tcpstart(cp, TCP_ACTIVE, Streamhi, 0);
		else if(cp->stproto == &ilinfo)
			ilstart(cp, IL_ACTIVE, 10);

	}
	else if(strcmp(field[0], "announce") == 0) {
		if((cp->stproto == &tcpinfo && cp->tcpctl.state != CLOSED) ||
		   (cp->stproto == &ilinfo && cp->ilctl.state != Ilclosed))
				error(Edevbusy);

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
	Port   dport;
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

	/* Look for a conversation structure for this port */
	etab = &muxed[conf.ip];
	for(ifc = muxed; ifc < etab; ifc++) {
		if(ifc->psrc == dport && ifc->ref) {
			/* Trim the packet down to data size */
			len = len - (UDP_HDRSIZE-UDP_PHDRSIZE);
			bp = btrim(bp, UDP_EHSIZE+UDP_HDRSIZE, len);
			if(bp == 0)
				return;

			/* Stuff the src address into the remote file */
		 	ifc->dst = addr;
			ifc->pdst = nhgets(uh->udpsport);
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
	hnputs(uh->udpplen, ptcllen);
	hnputl(uh->udpdst, ipc->dst);
	hnputl(uh->udpsrc, Myip);
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
	ipc->ref = 0;

	closeipifc(ipc->ipinterface);
	netdisown(&ipc->ipinterface->net, ipc->index);
}

void
udpstopen(Queue *q, Stream *s)
{
	Ipconv *ipc;

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_UDPPROTO, udprcvmsg, ipconv[s->dev],
			            1500, 512, ETHER_HDR, "UDP");

	ipc->ref = 1;
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

	s = (Ipconv *)(q->ptr);
	len = blen(bp);
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
	case LISTEN:
		tcb->flags |= ACTIVE;
		send_syn(tcb);
		setstate(s, SYN_SENT);
		/* No break */
	case SYN_SENT:
	case SYN_RECEIVED:
	case ESTABLISHED:
	case CLOSE_WAIT:
		qlock(tcb);
		appendb(&tcb->sndq, bp);
		tcb->sndcnt += len;
		tcprcvwin(s);
		tcp_output(s);
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

	ipc->ref = 1;

	ipc->readq = RD(q);
	ipc->readq->rp = &tcpflowr;

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
	sprint(buf, "%d.%d.%d.%d %d\n", fmtaddr(Myip), cp->psrc);
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
		sprint(buf, "il/%d %d %s\n", connection, cp->ref,
			ilstate[cp->ilctl.state]);
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

	if((s->stproto == &tcpinfo && s->tcpctl.state != LISTEN) ||
	   (s->stproto == &ilinfo && s->ilctl.state != Illistening))
		errors("not announced");

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
				s->curlog--;
				new->newcon = 0;
				qunlock(&s->listenq);
				return new - base;
			}
		}
	}
}

void
tcpstclose(Queue *q)
{
	Ipconv *s;
	Tcpctl *tcb;

	s = (Ipconv *)(q->ptr);
	tcb = &s->tcpctl;

	s->ref = 0;

	/* Not interested in data anymore */
	s->readq = 0;

	switch(tcb->state){
	case CLOSED:
	case LISTEN:
	case SYN_SENT:
		close_self(s, 0);
		break;
	case SYN_RECEIVED:
	case ESTABLISHED:
		tcb->sndcnt++;
		tcb->snd.nxt++;
		setstate(s, FINWAIT1);
		goto output;
	case CLOSE_WAIT:
		tcb->sndcnt++;
		tcb->snd.nxt++;
		setstate(s, LAST_ACK);
	output:
		qlock(tcb);
		tcp_output(s);
		qunlock(tcb);
		break;
	}
	netdisown(&s->ipinterface->net, s->index);
}


/* 
 * ptcl_csum - protcol checksum routine
 */
ushort
ptcl_csum(Block *bp, int offset, int len)
{
	uchar *addr;
	ulong losum = 0, hisum = 0;
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
	odd = 0;
	while(len) {
		if(odd) {
			losum += *addr++;
			blen--;
			len--;
			odd = 0;
		}
		for(x = MIN(len, blen); x > 1; x -= 2) {
			hisum += addr[0];
			losum += addr[1];
			len -= 2;
			blen -= 2;
			addr += 2;
		}
		if(blen && x) {
			odd = 1;
			hisum += addr[0];
			len--;
		}
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

	losum &= 0xffff;

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

	etab = &ic[conf.ip];
	for(ifc = ic; ifc < etab; ifc++) {
		if(ifc->psrc == port) 
			return ifc;
	}

	return 0;
}

Port
nextport(Ipconv *ic, Port base)
{
	Port i;

	for(i = base; i < PORTMAX; i++) {
		if(!portused(ic, i))
			return(i);
	}
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
		if(s->psrc == dst && s->pdst == src &&
		   (s->dst == dest || dest == 0))
			return s;
	}

	return 0;
}
