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

#define	 DBG	if(0)print
int 		ilcksum = 1;
static 	int 	initseq = 25000;
static	Rendez	ilackr;

char	*ilstate[] = { "Closed", "Syncer", "Syncee", "Established", "Listening", "Closing" };
char	*iltype[] =  { "sync", "data", "dataquerey", "ack", "querey", "state", "close" };
static char *etime = "connection timed out";

/* Always Acktime < Fasttime < Slowtime << Ackkeepalive */
enum
{
	Iltickms 	= 100,
	Slowtime 	= 200*Iltickms,
	Fasttime 	= 4*Iltickms,
	Acktime		= 2*Iltickms,
	Ackkeepalive	= 6000*Iltickms,
	Defaultwin	= 20,
};

#define Starttimer(s)	{(s)->timeout = 0; (s)->fasttime = Fasttime;}
/* Packet dropping putnext for testing */
#define DPUTNEXT(q, b)	if((MACHP(0)->ticks&7) != 3)PUTNEXT(q, b);else{freeb(b);print(".");}

void	ilrcvmsg(Ipconv*, Block*);
void	ilackproc(void*);
void	ilsendctl(Ipconv*, Ilhdr*, int, ulong, ulong);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Ipconv*, Ilhdr*, Block*);
void	ilpullup(Ipconv*);
void	ilhangup(Ipconv*, char *);
void	ilfreeq(Ilcb*);
void	ilrexmit(Ilcb*);
void	ilbackoff(Ilcb*);

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
		ilsendctl(s, 0, Ilclose, ic->next, ic->recvd);
		break;
	case Illistening:
		ic->state = Ilclosed;
		s->psrc = 0;
		s->pdst = 0;
		s->dst = 0;
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

	ic = &ipc->ilctl;
	switch(ic->state) {
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
	ic->acktime = Ackkeepalive;

	PUTNEXT(q, bp);
}

void
ilackq(Ilcb *ic, Block *bp)
{
	Block *np;

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyb(bp, blen(bp));
	qlock(&ic->ackq);
	if(ic->unacked)
		ic->unackedtail->list = np;
	else {
		/* Start timer since we may have been idle for some time */
		Starttimer(ic);
		ic->unacked = np;
	}
	ic->unackedtail = np;
	qunlock(&ic->ackq);
	np->list = 0;
}

void
ilackto(Ilcb *ic, ulong ackto)
{
	Ilhdr *h;
	Block *bp;
	ulong id;

	qlock(&ic->ackq);
	while(ic->unacked) {
		h = (Ilhdr *)ic->unacked->rptr;
		id = nhgetl(h->ilid);
		if(ackto < id)
			break;

		bp = ic->unacked;
		ic->unacked = bp->list;
		bp->list = 0;
		freeb(bp);
	}
	qunlock(&ic->ackq);
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
	char *st;

	ih = (Ilhdr *)bp->rptr;
	plen = blen(bp);
	if(plen < IL_EHSIZE+IL_HDRSIZE)
		goto drop;

	illen = nhgets(ih->illen);
	if(illen+IL_EHSIZE > plen)
		goto drop;

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	dst = nhgetl(ih->src);

	if(ilcksum && ptcl_csum(bp, IL_EHSIZE, illen) != 0) {
		st = (ih->iltype < 0 || ih->iltype > Ilclose) ? "?" : iltype[ih->iltype];
		print("il: cksum error, pkt(%s id %d ack %d %d.%d.%d.%d/%d->%d)\n",
			st, nhgetl(ih->ilid), nhgetl(ih->ilack), fmtaddr(dst), sp, dp);
		goto drop;
	}

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

	/* Look for a listener */
	for(s = ipc; s < etab; s++) {
		if(s->ilctl.state == Illistening)
		if(s->pdst == 0)
		if(s->dst == 0) {
			if(s->curlog > s->backlog)
				goto reset;

			new = ipincoming(ipc, s);
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
			ic->start = initseq;
			ic->next = ic->start+1;
			ic->recvd = 0;
			ic->rstart = nhgetl(ih->ilid);
			ic->slowtime = Slowtime;
			ic->window = Defaultwin;
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
	ilsendctl(0, ih, Ilclose, 0, 0);
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
				ic->state = Ilclosed;
				ilhangup(s, "connection rejected");
			}
			else {
				ic->recvd = id;
				ic->rstart = id;
				ilsendctl(s, 0, Ilack, ic->next, ic->recvd);
				ic->state = Ilestablished;
				ilpullup(s);
				Starttimer(ic);
			}
			break;
		case Ilclose:
			if(ack == ic->start) {
				ic->state = Ilclosed;
				ilhangup(s, "remote close");
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
				ilsendctl(s, 0, Ilsync, ic->start, ic->recvd);
				Starttimer(ic);
			}
			break;
		case Ilack:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ilpullup(s);
				Starttimer(ic);
			}
			break;
		case Ilclose:
			if(id == ic->next) {
				ic->state = Ilclosed;
				ilhangup(s, "remote close");
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
				ilhangup(s, "remote close");
			}
			else {
				ilsendctl(s, 0, Ilack, ic->next, ic->rstart);
				Starttimer(ic);
			}
			freeb(bp);	
			break;
		case Ildata:
			Starttimer(ic);
			ilackto(ic, ack);
			ic->acktime = Acktime;
			iloutoforder(s, h, bp);
			ilpullup(s);
			break;
		case Ildataquery:
			Starttimer(ic);
			ilackto(ic, ack);
			ic->acktime = Acktime;
			iloutoforder(s, h, bp);
			ilpullup(s);
			ilsendctl(s, 0, Ilstate, ic->next, ic->recvd);
			break;
		case Ilack:
			ilackto(ic, ack);
			Starttimer(ic);
			freeb(bp);
			break;
		case Ilquerey:
			ilackto(ic, ack);
			ilsendctl(s, 0, Ilstate, ic->next, ic->recvd);
			Starttimer(ic);
			freeb(bp);
			break;
		case Ilstate:
			ilackto(ic, ack);
			ilrexmit(ic);
			Starttimer(ic);
			freeb(bp);
			break;
		case Ilclose:
			freeb(bp);
			if(ack < ic->start || ack > ic->next) 
				break;
			ilsendctl(s, 0, Ilclose, ic->next, ic->recvd);
			ic->state = Ilclosing;
			ilfreeq(ic);
			Starttimer(ic);
			break;
		}
		break;
	case Illistening:
		freeb(bp);
		break;
	case Ilclosing:
		switch(h->iltype) {
		case Ilclose:
			ic->recvd = id;
			ilsendctl(s, 0, Ilclose, ic->next, ic->recvd);
			if(ack == ic->next) {
				ic->state = Ilclosed;
				ilhangup(s, 0);
			}
			Starttimer(ic);
			break;
		default:
			break;
		}
		freeb(bp);
		break;
	}
}

void
ilrexmit(Ilcb *ic)
{
	Block *nb;
	Ilhdr *h;

	if(ic->unacked == 0)
		return;

	nb = copyb(ic->unacked, blen(ic->unacked));
	h = (Ilhdr*)nb->rptr;
	DBG("rxmit %d.", nhgetl(h->ilid));

	h->iltype = Ildataquery;
	hnputl(h->ilack, ic->recvd);
	h->ilsum[0] = 0;
	h->ilsum[1] = 0;
	if(ilcksum)
		hnputs(h->ilsum, ptcl_csum(nb, IL_EHSIZE, nhgets(h->illen)));

	PUTNEXT(Ipoutput, nb);
}

/* DEBUG */
void
ilprocess(Ipconv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic = &s->ilctl;

	USED(ic);
	DBG("%11s rcv %d/%d snt %d/%d pkt(%s id %d ack %d %d->%d) ",
		ilstate[ic->state],  ic->rstart, ic->recvd, ic->start, ic->next,
		iltype[h->iltype], nhgetl(h->ilid), nhgetl(h->ilack), 
		nhgets(h->ilsrc), nhgets(h->ildst));

	_ilprocess(s, h, bp);

	DBG("%11s rcv %d snt %d\n", ilstate[ic->state], ic->recvd, ic->next);
}

void
ilhangup(Ipconv *s, char *msg)
{
	Block *nb;
	int l;

	DBG("hangup! %s %d/%d\n", msg ? msg : "??", s->psrc, s->pdst);
	if(s->readq) {
		if(msg) {
			l = strlen(msg);
			nb = allocb(l);
			strcpy((char*)nb->wptr, msg);
			nb->wptr += l;
		}
		else
			nb = allocb(0);
		nb->type = M_HANGUP;
		nb->flags |= S_DELIM;
		PUTNEXT(s->readq, nb);
	}
	s->psrc = 0;
	s->pdst = 0;
	s->dst = 0;
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

	qlock(&ic->outo);
	while(ic->outoforder) {
		bp = ic->outoforder;
		oh = (Ilhdr*)bp->rptr;
		oid = nhgetl(oh->ilid);
		if(oid <= ic->recvd) {
			ic->outoforder = bp->list;
			freeb(bp);
			continue;
		}
		if(oid != ic->recvd+1)
			break;

		ic->recvd = oid;
		ic->outoforder = bp->list;
		ic->oblks--;

		qunlock(&ic->outo);
		bp->list = 0;
		dlen = nhgets(oh->illen)-IL_HDRSIZE;
		bp = btrim(bp, IL_EHSIZE+IL_HDRSIZE, dlen);
		PUTNEXT(s->readq, bp);
		qlock(&ic->outo);
	}
	qunlock(&ic->outo);
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


	id = nhgetl(h->ilid);
	/* Window checks */
	if(id <= ic->recvd || ic->oblks > ic->window) {
		freeb(bp);
		return;
	}

	/* Packet is acceptable so sort onto receive queue for pullup */
	qlock(&ic->outo);
	ic->oblks++;
	if(ic->outoforder == 0)
		ic->outoforder = bp;
	else {
		l = &ic->outoforder;
		for(f = *l; f; f = f->list) {
			lid = ((Ilhdr*)(f->rptr))->ilid;
			if(id < nhgetl(lid)) {
				bp->list = f;
				*l = bp;
				qunlock(&ic->outo);
				return;
			}
			l = &f->list;
		}
		*l = bp;
	}
	qunlock(&ic->outo);
}

void
ilsendctl(Ipconv *ipc, Ilhdr *inih, int type, ulong id, ulong ack)
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
		hnputl(ih->ilid, id);
		hnputl(ih->ilack, ack);
		ic->acktime = Ackkeepalive;
	}
	ih->iltype = type;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptcl_csum(bp, IL_EHSIZE, IL_HDRSIZE));

/*	DBG("\nctl(%s id %d ack %d %d->%d)\n",
		iltype[ih->iltype], nhgetl(ih->ilid), nhgetl(ih->ilack), 
		nhgets(ih->ilsrc), nhgets(ih->ildst));
*/
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
		tsleep(&ilackr, return0, 0, Iltickms);
		for(s = base; s < end; s++) {
			ic = &s->ilctl;
			ic->timeout += Iltickms;
			switch(ic->state) {
			case Ilclosed:
			case Illistening:
				break;
			case Ilclosing:
				if(ic->timeout >= ic->fasttime) {
					ilsendctl(s, 0, Ilclose, ic->next, ic->recvd);
					ilbackoff(ic);
				}
				if(ic->timeout >= ic->slowtime) {
					ic->state = Ilclosed;
					ilhangup(s, 0);
				}
				break;
			case Ilsyncee:
			case Ilsyncer:
				if(ic->timeout >= ic->fasttime) {
					ilsendctl(s, 0, Ilsync, ic->start, ic->recvd);
					ilbackoff(ic);
				}
				if(ic->timeout >= ic->slowtime) {
					ic->state = Ilclosed;
					ilhangup(s, etime);
				}
				break;
			case Ilestablished:
				ic->acktime -= Iltickms;
				if(ic->acktime <= 0)
					ilsendctl(s, 0, Ilack, ic->next, ic->recvd);
				if(ic->unacked == 0) {
					ic->timeout = 0;
					break;
				}
				if(ic->timeout >= ic->fasttime) {
					ilrexmit(ic);
					ilbackoff(ic);
				}
				if(ic->timeout >= ic->slowtime) {
					ic->state = Ilclosed;
					ilhangup(s, etime);
					break;
				}
				break;
			}
		}
	}
}

void
ilbackoff(Ilcb *ic)
{
	if(ic->fasttime < Slowtime/2)
		ic->fasttime += Fasttime;
	else
		ic->fasttime = (ic->fasttime)*3/2;
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
	ic->slowtime = Slowtime;


	initseq += TK2MS(MACHP(0)->ticks);
	ic->start = initseq;
	ic->next = ic->start+1;
	ic->recvd = 0;
	ic->window = window;
	ic->oblks = 0;

	switch(type) {
	case IL_PASSIVE:
		ic->state = Illistening;
		break;
	case IL_ACTIVE:
		ic->state = Ilsyncer;
		ilsendctl(ipc, 0, Ilsync, ic->start, ic->recvd);
		break;
	}
}

void
ilfreeq(Ilcb *ic)
{
	Block *bp, *next;

	qlock(&ic->ackq);
	for(bp = ic->unacked; bp; bp = next) {
		next = bp->list;
		freeb(bp);
	}
	ic->unacked = 0;
	qunlock(&ic->ackq);

	qlock(&ic->outo);
	for(bp = ic->outoforder; bp; bp = next) {
		next = bp->list;
		freeb(bp);
	}
	ic->outoforder = 0;
	qunlock(&ic->outo);
}
