/*
 * stil - Internet link protocol
 */
#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"arp.h"
#include 	"ipdat.h"

int 		ilcksum = 1;
static 	int 	initseq = 25000;
static	Rendez	ilackr;
Rendez poor;	/* DEBUG */
char	*ilstate[] = { "Closed", "Syncer", "Syncee", "Established", "Listening", "Closing" };
char	*iltype[] =  { "sync", "data", "dataquerey", "ack", "querey", "state", "close" };

enum
{
	Slowtime = 20,
	Fasttime = 1,
};

void	ilrcvmsg(Ipconv*, Block*);
void	ilackproc(void*);
void	ilsendctl(Ipconv*, Ilhdr*, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Ipconv*, Ilhdr*, Block*);
void	ilpullup(Ipconv*);
void	ilhangup(Ipconv*);
void	ilfreeq(Ilcb*);

void
ilopen(Queue *q, Stream *s)
{
	Ipconv *ipc;
	static int ilkproc;

	if(!Ipoutput) {
		Ipoutput = WR(q);
		s->opens++;
		s->inuse++;
	}

	if(ilkproc == 0) {
		ilkproc = 1;
		kproc("ilack", ilackproc, ipconv[s->dev]);
	}

	ipc = &ipconv[s->dev][s->id];
	ipc->ipinterface = newipifc(IP_ILPROTO, ilrcvmsg, ipconv[s->dev],
			            1500, 512, ETHER_HDR, "IL");

	qlock(ipc);
	ipc->ref++;
	qunlock(ipc);
	ipc->readq = RD(q);	
	RD(q)->ptr = (void *)ipc;
	WR(q)->next->ptr = (void *)ipc->ipinterface;
	WR(q)->ptr = (void *)ipc;
}

void
ilclose(Queue *q)
{
	Ipconv *s;
	Ilcb *ic;
	Block *bp, *next;

	s = (Ipconv *)(q->ptr);
	ic = &s->ilctl;
	qlock(s);
	s->ref--;
	s->readq = 0;
	qunlock(s);

	switch(ic->state) {
	case Ilclosing:
	case Ilclosed:
		break;
	case Ilsyncer:
	case Ilsyncee:
	case Ilestablished:
		ilfreeq(ic);
		ic->state = Ilclosing;
		ilsendctl(s, 0, Ilclose);
		break;
	Illistening:
		ic->state = Ilclosed;
		break;
	}
}

void
iloput(Queue *q, Block *bp)
{
	Ipconv *ipc;
	Ilhdr *ih;
	Ilcb *ic;
	int dlen;
	Block *np, *f;

	ipc = (Ipconv *)(q->ptr);
	if(ipc->psrc == 0)
		error(Enoport);

	switch(ipc->ilctl.state) {
	case Ilclosed:
	case Illistening:
	case Ilclosing:
		error(Ehungup);
	}

	if(bp->type != M_DATA) {
		freeb(bp);
		error(Ebadctl);
	}

	/* Only allow atomic Il writes to form datagrams */
	for(f = bp; f->next; f = f->next)
		;
	if((f->flags & S_DELIM) == 0) {
		freeb(bp);
		error(Emsgsize);
	}

	dlen = blen(bp);
	if(dlen > IL_DATMAX) {
		freeb(bp);
		error(Emsgsize);
	}

	/* Make space to fit il & ip & ethernet header */
	bp = padb(bp, IL_EHSIZE+IL_HDRSIZE);

	ih = (Ilhdr *)(bp->rptr);
	ic = &ipc->ilctl;

	/* Ip fields */
	hnputl(ih->src, Myip);
	hnputl(ih->dst, ipc->dst);
	ih->proto = IP_ILPROTO;
	/* Il fields */
	hnputs(ih->illen, dlen+IL_HDRSIZE);
	hnputs(ih->ilsrc, ipc->psrc);
	hnputs(ih->ildst, ipc->pdst);
	hnputl(ih->ilid, ic->next++);
	hnputl(ih->ilack, ic->recvd);
	ih->iltype = Ildata;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	/* Checksum of ilheader plus data (not ip & no pseudo header) */
	if(ilcksum)
		hnputs(ih->ilsum, ptcl_csum(bp, IL_EHSIZE, dlen+IL_HDRSIZE));

	ilackq(ic, bp);
	delay(100);
	PUTNEXT(q, bp);
}

void
ilackq(Ilcb *ic, Block *bp)
{
	Block *np;

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyb(bp, blen(bp));
	if(ic->unacked)
		ic->unackedtail->list = np;
	else 
		ic->unacked = np;
	ic->unackedtail = np;
	np->list = 0;
}

void
ilackto(Ilcb *ic, ulong ackto)
{
	Ilhdr *h;
	Block *bp;
	ulong ack;

	while(ic->unacked) {
		h = (Ilhdr *)ic->unacked->rptr;
		ack = nhgetl(h->ilack);
		if(ackto < ack)
			break;
		ic->lastack = ackto;
		bp = ic->unacked;
		ic->unacked = bp->list;
		bp->list = 0;
		freeb(bp);
	}
}

void
iliput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

void
ilrcvmsg(Ipconv *ipc, Block *bp)
{
	Ilhdr *ih;
	Ilcb *ic;
	int plen, illen;
	Ipconv *s, *etab, *new;
	short sp, dp;
	Ipaddr dst;

	ih = (Ilhdr *)bp->rptr;

	plen = blen(bp);
	if(plen < IL_EHSIZE+IL_HDRSIZE)
		goto drop;

	illen = nhgets(ih->illen);
	if(illen+IL_EHSIZE > plen)
		goto drop;

	if(ilcksum && ptcl_csum(bp, IL_EHSIZE, illen) != 0) {
		print("il: cksum error\n");
		goto drop;
	}

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	dst = nhgetl(ih->src);

	etab = &ipc[conf.ip];
	for(s = ipc; s < etab; s++)
		if(s->psrc == sp)
		if(s->pdst == dp)
		if(s->dst == dst) {
			ilprocess(s, ih, bp);
			return;
		}

	if(ih->iltype != Ilsync)
		goto drop;

	if(s->curlog > s->backlog)
		goto reset;

	/* Look for a listener */
	for(s = ipc; s < etab; s++) {
		if(s->ilctl.state == Illistening)
		if(s->pdst == 0)
		if(s->dst == 0) {
			new = ipincoming(ipc);
			if(new == 0)
				goto reset;

			new->newcon = 1;
			new->ipinterface = s->ipinterface;
			new->psrc = sp;
			new->pdst = dp;
			new->dst = nhgetl(ih->src);

			ic = &new->ilctl;
			ic->state = Ilsyncee;
			initseq += TK2MS(MACHP(0)->ticks);
			ic->next = initseq;
			ic->start = ic->next;
			ic->recvd = 0;
			ic->rstart = nhgetl(ih->ilid);
			ilprocess(new, ih, bp);

			s->ipinterface->ref++;
			s->curlog++;
			wakeup(&s->listenr);
			return;
		}
	}
drop:
	print("drop\n");
	freeb(bp);
	return;
reset:
	print("reset\n");
	ilsendctl(0, ih, Ilclose);
	freeb(bp);
}

void
_ilprocess(Ipconv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	Block *nb, *next;
	ulong id, ack, dlen;

	id = nhgetl(h->ilid);
	ack = nhgetl(h->ilack);
	ic = &s->ilctl;

	switch(ic->state) {
	default:
		panic("il unknown state");
	case Ilclosed:
		freeb(bp);
		break;
	case Ilsyncer:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(ack != ic->start) {
				ilhangup(s);
				ic->state = Ilclosed;
			}
			else {
				ic->recvd = id;
				ic->rstart = id;
				ilsendctl(s, 0, Ilack);
				ic->state = Ilestablished;
				ilpullup(s);
			}
			break;
		case Ilclose:
			if(ack == ic->start) {
				ic->state = Ilclosed;
				ilhangup(s);
			}
			break;
		}
		freeb(bp);
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
				ilsendctl(s, 0, Ilsync);
			}
			break;
		case Ilack:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ilpullup(s);
			}
			break;
		case Ilclose:
			if(ack == ic->start) {
				ic->state = Ilclosed;
				ilhangup(s);
			}
			break;
		}
		freeb(bp);
		break;
	case Ilestablished:
		switch(h->iltype) {
		case Ilsync:
			if(id != ic->start) {
				ic->state = Ilclosed;
				ilhangup(s);
			}
			else 
				ilsendctl(s, 0, Ilack);
			freeb(bp);	
			break;
		case Ildata:
		case Ildataquery:
			if(id < ic->recvd) {
				freeb(bp);
				break;
			}
			if(ack >= ic->recvd)
				ilackto(ic, ack);
			iloutoforder(s, h, bp);
			ilpullup(s);
			if(h->iltype == Ildataquery)
				ilsendctl(s, 0, Ilstate);
			break;
		case Ilack:
			ilackto(ic, ack);
			freeb(bp);
			break;
		case Ilquerey:
			ilackto(ic, ack);
			ilsendctl(s, 0, Ilstate);
			freeb(bp);
			break;
		case Ilstate:
			ilackto(ic, ack);
			if(ic->unacked) {
				nb = copyb(ic->unacked, blen(ic->unacked));
				h = (Ilhdr*)nb;
				h->iltype = Ildataquery;
				hnputl(h->ilack, ic->recvd);
				PUTNEXT(Ipoutput, nb);
			}
			freeb(bp);
			break;
		case Ilclose:
			freeb(bp);
			if(id != ic->recvd)
				break;
			ilsendctl(s, 0, Ilclose);
			ic->state = Ilclosing;
			ilfreeq(ic);
			break;
		}
		break;
	case Illistening:
		freeb(bp);
		break;
	case Ilclosing:
		switch(h->iltype) {
		case Ilclose:
			if(ack == ic->next) {
				ic->state = Ilclosed;
				ilhangup(s);
			}
			else {
				ic->recvd = id;
				ilsendctl(s, 0, Ilclose);
			}
			break;
		default:
			ilsendctl(s, 0, Ilclose);
			ilhangup(s);
			break;
		}
		freeb(bp);
		break;
	}
}

/* DEBUG */
void
ilprocess(Ipconv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic = &s->ilctl;

	print("%s rcv %d/%d snt %d/%d pkt(%s id %d ack %d %d->%d) ",
		ilstate[ic->state],  ic->rstart, ic->recvd, ic->start, ic->next,
		iltype[h->iltype], nhgetl(h->ilid), nhgetl(h->ilack), 
		nhgets(h->ilsrc), nhgets(h->ildst));

	_ilprocess(s, h, bp);

	print("%s rcv %d snt %d\n", ilstate[ic->state], ic->recvd, ic->next);
}

void
ilhangup(Ipconv *s)
{
	Block *nb;

	if(s->readq) {
		nb = allocb(0);
		nb->type = M_HANGUP;
		nb->flags |= S_DELIM;
		PUTNEXT(s->readq, nb);
	}
}

void
ilpullup(Ipconv *s)
{
	Ilcb *ic;
	Ilhdr *oh;
	ulong oid, dlen;
	Block *bp;

	if(s->readq == 0)
		return;

	ic = &s->ilctl;
	if(ic->state != Ilestablished)
		return;

	while(ic->outoforder) {
		bp = ic->outoforder;
		oh = (Ilhdr*)bp->rptr;
		oid = nhgetl(oh->ilid);
		if(oid > ic->recvd)
			break;
		if(oid < ic->recvd) {
			ic->outoforder = bp->list;
			freeb(bp);
		}
		if(oid == ic->recvd) {
			ic->recvd++;
			ic->outoforder = bp->list;
			bp->list = 0;
			dlen = nhgets(oh->illen)-IL_HDRSIZE;
			bp = btrim(bp, IL_EHSIZE+IL_HDRSIZE, dlen);
			PUTNEXT(s->readq, bp);
		}
	}
}

void
iloutoforder(Ipconv *s, Ilhdr *h, Block *bp)
{
	Block *f, **l;
	Ilcb *ic;
	ulong id;
	uchar *lid;

	ic = &s->ilctl;
	bp->list = 0;
	if(ic->outoforder == 0) {
		ic->outoforder = bp;
		return;
	}

	id = nhgetl(h->id);
	l = &ic->outoforder;
	for(f = *l; f; f = f->list) {
		lid = ((Ilhdr*)(bp->rptr))->ilid;
		if(id > nhgetl(lid))
			break;
		l = &f->list;
	}
	bp->list = *l;
	*l = bp;
}

void
ilsendctl(Ipconv *ipc, Ilhdr *inih, int type)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;
	ulong id;

	bp = allocb(IL_EHSIZE+IL_HDRSIZE);
	bp->wptr += IL_EHSIZE+IL_HDRSIZE;
	bp->flags |= S_DELIM;

	ih = (Ilhdr *)(bp->rptr);
	ic = &ipc->ilctl;

	/* Ip fields */
	ih->proto = IP_ILPROTO;
	hnputl(ih->src, Myip);
	hnputs(ih->illen, IL_HDRSIZE);
	if(inih) {
		hnputl(ih->dst, nhgetl(inih->src));
		hnputs(ih->ilsrc, nhgets(inih->ildst));
		hnputs(ih->ildst, nhgets(inih->ilsrc));
		hnputl(ih->ilid, nhgetl(inih->ilack));
		hnputl(ih->ilack, nhgetl(inih->ilid));
	}
	else {
		hnputl(ih->dst, ipc->dst);
		hnputs(ih->ilsrc, ipc->psrc);
		hnputs(ih->ildst, ipc->pdst);
		id = ic->next;
		if(type == Ilsync)
			id = ic->start;
		hnputl(ih->ilid, id);
		hnputl(ih->ilack, ic->recvd);
	}
	ih->iltype = type;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptcl_csum(bp, IL_EHSIZE, IL_HDRSIZE));

	print("ctl(%s id %d ack %d %d->%d) ",
		iltype[ih->iltype], nhgetl(ih->ilid), nhgetl(ih->ilack), 
		nhgets(ih->ilsrc), nhgets(ih->ildst));

	PUTNEXT(Ipoutput, bp);
}

void
ilackproc(void *a)
{
	Ipconv *base, *end, *s;
	Ilcb *ic;
	Block *bp, *np;

	base = (Ipconv*)a;
	end = &base[conf.ip];

	for(;;) {
		tsleep(&ilackr, return0, 0, 250);
		for(s = base; s < end; s++) {
			ic = &s->ilctl;
			switch(ic->state) {
			case Ilclosed:
			case Illistening:
				break;
			case Ilclosing:
				break;
			case Ilsyncee:
				break;
			case Ilsyncer:
				break;
			case Ilestablished:
				break;
			}
		}
	}
}

void
ilstart(Ipconv *ipc, int type, int window)
{
	Ilcb *ic = &ipc->ilctl;

	if(ic->state != Ilclosed)
		return;

	ic->timeout = 0;
	ic->unacked = 0;
	ic->outoforder = 0;
	initseq += TK2MS(MACHP(0)->ticks);
	ic->next = initseq;
	ic->start = ic->next;
	ic->recvd = 0;
	ic->lastack = ic->next;
	ic->window = window;

	switch(type) {
	case IL_PASSIVE:
		ic->state = Illistening;
		break;
	case IL_ACTIVE:
		ic->state = Ilsyncer;
		ilsendctl(ipc, 0, Ilsync);
		break;
	}
}

void
ilfreeq(Ilcb *ic)
{
	Block *bp, *next;

	for(bp = ic->unacked; bp; bp = next) {
		next = bp->list;
		freeb(bp);
	}
	for(bp = ic->outoforder; bp; bp = next) {
		next = bp->list;
		freeb(bp);
	}
	ic->unacked = 0;
	ic->outoforder = 0;
}
