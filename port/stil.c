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

#define DPRINT if(pip)print
int 		ilcksum = 1;
static 	int 	initseq = 25000;
static	Rendez	ilackr;
char	*ilstate[] = { "Closed", "Syncer", "Syncee", "Established", "Listening", "Closing" };

enum
{
	Slowtime = 20,
	Fasttime = 1,
};

void	ilrcvmsg(Ipconv*, Block*);
void	ilackproc(void*);
void	ilsendctl(Ipconv*, Ilhdr*, int, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Ipconv*, Ilhdr*, Block*);
void	ilpullup(Ipconv*);
void	ilhangup(Ipconv*);

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
		for(bp = ic->outoforder; bp; bp = next) {
			next = bp->list;
			freeb(bp);
		}
		ic->outoforder = 0;
		ic->state = Ilclosing;
		ilsendctl(s, 0, Ilclose, 0);
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
	hnputl(ih->ilid, ic->sent++);
	hnputl(ih->ilack, ic->recvd);
	ih->iltype = Ildata;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	/* Checksum of ilheader plus data (not ip & no pseudo header) */
	if(ilcksum)
		hnputs(ih->ilsum, ptcl_csum(bp, IL_EHSIZE, dlen+IL_HDRSIZE));

	ilackq(ic, bp);
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

	while(ic->unacked) {
		h = (Ilhdr *)ic->unacked->rptr;
		if(ackto < nhgetl(h->ilack))
			break;
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
			new->ilctl.state = Ilsyncee;
			initseq += TK2MS(MACHP(0)->ticks);
			new->ilctl.sent = initseq;
			new->dst = nhgetl(ih->src);
			ilprocess(new, ih, bp);

			s->ipinterface->ref++;
			s->curlog++;
			wakeup(&s->listenr);
			return;
		}
	}
drop:
	freeb(bp);
	return;
reset:
	ilsendctl(0, ih, Ilclose, 0);
	freeb(bp);
}

void
ilprocess(Ipconv *s, Ilhdr *h, Block *bp)
{
	Block *nb;
	Ilcb *ic;
	ulong id, ack, dlen;

	id = nhgetl(h->ilid);
	ack = nhgetl(h->ilack);
	ic = &s->ilctl;

	ic->timeout = 0;
	/* Active transition machine - this tracks connection state */
	switch(ic->state) {
	case Ilsyncee:	
		switch(h->iltype) {
		case Ilsync:
			ic->recvd = id;
			ilsendctl(s, 0, Ilsync, 0);
			break;
		case Ilack:
			ic->state = Ilestablished;
			break;
		}
		break;
	case Ilsyncer:
		if(h->iltype == Ilsync && ic->start == ack) {
			ic->recvd = id+1;
			ilsendctl(s, 0, Ilack, 1);
			ic->state = Ilestablished;
			ilpullup(s);
		}
		break;
	case Ilclosing:
		ilsendctl(s, 0, Ilclose, 0);
		ic->state = Ilclosed;
		/* No break */
	case Ilclosed:
		ilhangup(s);
		freeb(bp);
		return;
	}

	/* Passive actions based on packet type */
	switch(h->iltype) {
	case Ilstate:
		if(ic->unacked) {
			nb = copyb(ic->unacked, blen(ic->unacked));
			PUTNEXT(Ipoutput, nb);	
		}
		else
			ilsendctl(s, 0, Ilack, 1);
		freeb(bp);
		break;
	case Ilack:
		ilackto(ic, ack);
		freeb(bp);
		break;
	case Ilquerey:
		ilsendctl(s, 0, Ilack, 1);
		freeb(bp);
		break;
	case Ildataquery:
		ilsendctl(s, 0, Ilack, 1);
		/* No break */
	case Ildata:
		ilackto(&s->ilctl, ack);
		switch(s->ilctl.state) {
		default:
			iloutoforder(s, h, bp);
			break;
		case Ilestablished:
			if(id < s->ilctl.recvd)
				freeb(bp);
			else if(id > s->ilctl.recvd)
				iloutoforder(s, h, bp);
			else if(s->readq) {
				s->ilctl.recvd++;
				bp->rptr += IL_EHSIZE+IL_HDRSIZE;
				PUTNEXT(s->readq, bp);
				ilpullup(s);
			}
		}
		break;
	case Ilclose:
		ic->state = Ilclosing;
		ilhangup(s);
		/* No break */
	default:
		freeb(bp);
		break;
	}
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
ilsendctl(Ipconv *ipc, Ilhdr *inih, int type, int ack)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;

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
		hnputl(ih->ilid, ic->sent);
		hnputl(ih->ilack, ic->recvd);
	}
	ih->iltype = type;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptcl_csum(bp, IL_EHSIZE, IL_HDRSIZE));

	if(ack == 0 && ipc) {
		ic->sent++;			/* Maybe needs locking */
		ilackq(&ipc->ilctl, bp);
	}
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
		tsleep(&ilackr, return0, 0, 100);
		for(s = base; s < end; s++) {
			ic = &s->ilctl;
			switch(ic->state) {
			case Ilclosed:
			case Illistening:
				break;
			case Ilclosing:
				break;
			case Ilsyncee:
			case Ilsyncer:
				ilsendctl(s, 0, Ilsync, 1);
				if(++ic->timeout == Slowtime) {
					ilhangup(s);
					ic->state = Ilclosed;
					s->dst = 0;
					s->pdst = 0;
					ic->timeout = 0;
				}
				break;
			case Ilestablished:
				if(++ic->timeout == Fasttime) {
					if(ic->lastack < ic->recvd)
						ilsendctl(s, 0, Ilstate, 1);
					ic->timeout = 0;
				}
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
	ic->sent = initseq;
	ic->start = ic->sent;
	ic->recvd = 0;
	ic->lastack = ic->sent;
	ic->window = window;

	switch(type) {
	case IL_PASSIVE:
		ic->state = Illistening;
		break;
	case IL_ACTIVE:
		ic->state = Ilsyncer;
		ilsendctl(ipc, 0, Ilsync, 1);
		break;
	}
}
