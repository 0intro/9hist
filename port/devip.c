#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include 	"arp.h"
#include 	"ipdat.h"

#include	"devtab.h"

enum{
	Nrprotocol = 2, /* Number of protocols supported by this driver */
	Nipsubdir = 4,	/* Number of subdirectory entries per connection */
};

int udpsum = 1;

Queue	*Tcpoutput;		/* Tcp to lance output channel */
Ipifc	*ipifc;			/* IP protocol interfaces for stip */
Ipconv	*ipconv[Nrprotocol];	/* Connections for each protocol */
Dirtab  *ipdir[Nrprotocol];	/* Connection directory structures */
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

Qinfo tcpinfo = { tcpstiput, tcpstoput, tcpstopen, tcpstclose, "tcp" };
Qinfo udpinfo = { udpstiput, udpstoput, udpstopen, udpstclose, "udp" };

Qinfo *protocols[] = { &tcpinfo, &udpinfo, 0 };

enum{
	ipdirqid,
	iplistenqid,
	iplportqid,
	iprportqid,
	ipstatusqid,
	ipchanqid,

	ipcloneqid
};

Dirtab ipsubdir[]={
	"listen",	{iplistenqid},		0,	0600,
	"local",	{iplportqid},		0,	0600,
	"remote",	{iprportqid},		0,	0600,
	"status",	{ipstatusqid},		0,	0600,
};

void
ipreset(void)
{
	int i;

	ipifc = (Ipifc *)ialloc(sizeof(Ipifc) * conf.ip, 0);

	for(i = 0; i < Nrprotocol; i++) {
		ipconv[i] = (Ipconv *)ialloc(sizeof(Ipconv) * conf.ip, 0);
		ipdir[i] = (Dirtab *)ialloc(sizeof(Dirtab) * (conf.ip+1), 0);
		ipmkdir(protocols[i], ipdir[i], ipconv[i]);
		newqinfo(protocols[i]);
	}

	initfrag(conf.frag);
	tcpinit();
}

void
ipmkdir(Qinfo *stproto, Dirtab *dir, Ipconv *cp)
{
	Dirtab *etab;
	int i;

	etab = &dir[conf.ip];
	for(i = 0; dir < etab; i++, cp++, dir++) {
		cp->stproto = stproto;
		sprint(dir->name, "%d", i);
		dir->qid.path = CHDIR|STREAMQID(i, ipchanqid);
		dir->qid.vers = 0;
		dir->length = 0;
		dir->perm = 0600;
	}

	/* Make the clone */
	strcpy(dir->name, "clone");
	dir->qid.path = ipcloneqid;
	dir->qid.vers = 0;
	dir->length = 0;
	dir->perm = 0600;
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
	if(c->qid.path == CHDIR)
		return devwalk(c, name, ipdir[c->dev], conf.ip+1, devgen);
	else
		return devwalk(c, name, ipsubdir, Nipsubdir, streamgen);
}

void
ipstat(Chan *c, char *db)
{
	if(c->qid.path == CHDIR)
		devstat(c, db, ipdir[c->dev], conf.ip+1, devgen);
	else if(c->qid.path == ipcloneqid)
		devstat(c, db, &ipdir[c->dev][conf.ip], 1, devgen);
	else
		devstat(c, db, ipsubdir, Nipsubdir, streamgen);
}

Chan *
ipopen(Chan *c, int omode)
{
	Ipconv *cp;

	cp = &ipconv[c->dev][STREAMID(c->qid.path)];
	if(c->qid.path & CHDIR) {
		if(omode != OREAD)
			error(Eperm);
	}
	else switch(STREAMTYPE(c->qid.path)) {
	case ipcloneqid:
		ipclonecon(c);
		break;
	case iplportqid:
	case iprportqid:
	case ipstatusqid:
		if(omode != OREAD)
			error(Ebadarg);
		break;
	case iplistenqid:
		if(cp->stproto != &tcpinfo)
			error(Eprotonosup);

		if(cp->backlog == 0)
			cp->backlog = 1;

		streamopen(c, &ipinfo);
		if(c->stream->devq->next->info != cp->stproto)
			pushq(c->stream, cp->stproto);

		if(cp->stproto == &tcpinfo)
			open_tcp(cp, TCP_PASSIVE, Streamhi, 0);
	
		iplisten(c, cp, ipconv[c->dev]);
		break;
	case Sdataqid:
		streamopen(c, &ipinfo);
		if(c->stream->devq->next->info != cp->stproto)
			pushq(c->stream, cp->stproto);

		if(cp->stproto == &tcpinfo)
			open_tcp(cp, TCP_ACTIVE, Streamhi, 0);
		break;
	case Sctlqid:
		streamopen(c, &ipinfo);
		if(c->stream->devq->next->info != cp->stproto)
			pushq(c->stream, cp->stproto);
		break;
	}

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

Ipconv *
ipclonecon(Chan *c)
{
	Ipconv *base, *new, *etab;

	base = ipconv[c->dev];
	etab = &base[conf.ip];
	for(new = base; new < etab; new++) {
		if(new->ref == 0 && canqlock(new)) {
			if(new->ref ||
		          (new->stproto == &tcpinfo &&
			   new->tcpctl.state != CLOSED)) {
				qunlock(new);
				continue;
			}
			new->ref++;
			c->qid.path = CHDIR|STREAMQID(new-base, ipchanqid);
			devwalk(c, "ctl", 0, 0, streamgen);
			qunlock(new);

			streamopen(c, &ipinfo);
			pushq(c->stream, new->stproto);
			new->ref--;
			return new;
		}	
	}

	error(Enodev);
}

void
ipcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
ipremove(Chan *c)
{
	error(Eperm);
}

void
ipwstat(Chan *c, char *dp)
{
	error(Eperm);
}

void
ipclose(Chan *c)
{
	if(c->qid.path != CHDIR)
		streamclose(c);
}

long
ipread(Chan *c, void *a, long n, ulong offset)
{
	int t, connection;
	Ipconv *cp;
	char buf[WORKBUF];

	t = STREAMTYPE(c->qid.path);
	if(t >= Slowqid)
		return streamread(c, a, n);

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, ipdir[c->dev], conf.ip+1, devgen);
	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, ipsubdir, Nipsubdir, streamgen);

	connection = STREAMID(c->qid.path);
	cp = &ipconv[c->dev][connection];

	switch(t) {
	case iprportqid:
		sprint(buf, "%d.%d.%d.%d %d\n", fmtaddr(cp->dst), cp->pdst);
		return stringread(c, a, n, buf, offset);
	case iplportqid:
		sprint(buf, "%d.%d.%d.%d %d\n", fmtaddr(Myip), cp->psrc);
		return stringread(c, a, n, buf, offset);
	case ipstatusqid:
		if(cp->stproto == &tcpinfo) {
			sprint(buf, "tcp/%d %d %s %s\n", connection,
				cp->ref, tcpstate[cp->tcpctl.state],
				cp->tcpctl.flags & CLONE ? "listen" : "connect");
		}
		else
			sprint(buf, "%s/%d %d\n", cp->stproto->name, 
				connection, cp->ref);

		return stringread(c, a, n, buf, offset);
	}

	error(Eperm);
}

long
ipwrite(Chan *c, char *a, long n, ulong offset)
{
	int 	m, backlog, type;
	char 	*field[5], buf[256];
	Ipconv  *cp;
	Port	port, base;

	type = STREAMTYPE(c->qid.path);
	if (type == Sdataqid)
		return streamwrite(c, a, n, 0); 

	if (type == Sctlqid) {
		cp = &ipconv[c->dev][STREAMID(c->qid.path)];

		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 5, ' ');

		if(strcmp(field[0], "connect") == 0) {
			if(cp->stproto == &tcpinfo &&
			   cp->tcpctl.state != CLOSED)
				error(Edevbusy);

			if(m != 2)
				error(Ebadarg);

			switch(getfields(field[1], field, 5, '!')) {
			default:
				error(Ebadarg);
			case 2:
				base = PORTALLOC;
				break;
			case 3:
				if(strcmp(field[2], "r") != 0)
					error(Eperm);
				base = PRIVPORTALLOC;
				break;
			}
			cp->dst = ipparse(field[0]);
			cp->pdst = atoi(field[1]);

			/* If we have no local port assign one */
			qlock(&ipalloc);
			if(cp->psrc == 0)
				cp->psrc = nextport(ipconv[c->dev], base);
			qunlock(&ipalloc);

		}
		else if(strcmp(field[0], "announce") == 0 ||
			strcmp(field[0], "reserve") == 0) {
				if(cp->stproto == &tcpinfo &&
				   cp->tcpctl.state != CLOSED)
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
			cp->ptype = *field[0];
			qunlock(&ipalloc);
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

	error(Eperm);
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
	int	dlen, ptcllen, newlen;

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

	/* If the port was bound rather than reserved, clear the allocation */
	qlock(ipc);
	if(--ipc->ref == 0 && ipc->ptype == 'b')
		ipc->psrc = 0;
	qunlock(ipc);

	closeipifc(ipc->ipinterface);
}

void
udpstopen(Queue *q, Stream *s)
{
	Ipconv *ipc;

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_UDPPROTO, udprcvmsg, ipconv[s->dev],
			            1500, 512, ETHER_HDR, "UDP");

	qlock(ipc);
	ipc->ref++;
	qunlock(ipc);
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
	int len, errnum, oob = 0;

	s = (Ipconv *)(q->ptr);
	len = blen(bp);
	tcb = &s->tcpctl;

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
		if(oob == 0) {
			appendb(&tcb->sndq, bp);
			tcb->sndcnt += len;
		}
		else {
			if(tcb->snd.up == tcb->snd.una)
				tcb->snd.up = tcb->snd.ptr;
			appendb(&tcb->sndoobq, bp);
			tcb->sndoobcnt += len;
		}

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
	static int donekproc;

	/* Start tcp service processes */
	if(!Tcpoutput) {
		Tcpoutput = WR(q);
		/* This never goes away - we use this queue to send acks/rejects */
		s->opens++;
		s->inuse++;
		/* Flow control and tcp timer processes */
		kproc("tcpack", tcpackproc, 0);
		kproc("tcpflow", tcpflow, &ipconv[s->dev]);

	}

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_TCPPROTO, tcp_input, ipconv[s->dev], 
			            1500, 512, ETHER_HDR, "TCP");

	qlock(ipc);
	ipc->ref++;
	qunlock(ipc);

	ipc->readq = RD(q);
	ipc->readq->rp = &tcpflowr;

	RD(q)->ptr = (void *)ipc;
	WR(q)->next->ptr = (void *)ipc->ipinterface;
	WR(q)->ptr = (void *)ipc;
}

int
tcp_havecon(Ipconv *s)
{
	return s->curlog;
}

void
iplisten(Chan *c, Ipconv *s, Ipconv *base)
{
	Ipconv *etab, *new;

	qlock(&s->listenq);

	for(;;) {
		sleep(&s->listenr, tcp_havecon, s);

		/* Search for the new connection, clone the control channel and
		 * return an open channel to the listener
		 */
		for(new = base, etab = &base[conf.ip]; new < etab; new++) {
			if(new->psrc == s->psrc && new->pdst != 0 && 
			   new->dst && (new->tcpctl.flags & CLONE) == 0) {
				new->ref++;

				/* Remove the listen channel reference */
				streamclose(c);

				s->curlog--;
				/* Attach the control channel to the new connection */
				c->qid.path = CHDIR|STREAMQID(new-base, ipchanqid);
				devwalk(c, "ctl", 0, 0, streamgen);
				streamopen(c, &ipinfo);
				pushq(c->stream, new->stproto);
				new->ref--;

				qunlock(&s->listenq);
				return;
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

	qlock(s);
	s->ref--;
	qunlock(s);

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
}


/* 
 * ptcl_csum - protcol cecksum routine
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

