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
int ilcksum = 1;

static int initseq = 25000;
char *ilstate[] = { "Closed", "Syncer", "Syncee", "Established", "Listening", "Closing" };

void	ilrcvmsg(Ipconv*, Block*);
void	ilackproc(void*);
void	ilsendctl(Ipconv*, Ilhdr*, int, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Ipconv*, Ilhdr*, Block*);

void
ilopen(Queue *q, Stream *s)
{
	Ipconv *ipc;
	static int ilkproc;

	/* Start il service processes */
	if(!Ipoutput) {
		Ipoutput = WR(q);
		/* This never goes away - we use this queue to send acks/rejects */
		s->opens++;
		s->inuse++;
	}

	if(ilkproc == 0) {
		ilkproc = 1;
		kproc("ilack", ilackproc, 0);
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
}

void
iloput(Queue *q, Block *bp)
{
	Ipconv *ipc;
	Ilhdr *ih;
	Ilcb *ic;
	int dlen;
	Block *np;

	ipc = (Ipconv *)(q->ptr);
	if(ipc->psrc == 0)
		error(Enoport);

	switch(ipc->ilctl.state) {
	case Ilclosed:
	case Ilsyncee:
	case Illistening:
	case Ilclosing:
		error(Ehungup);
	}

	if(bp->type != M_DATA) {
		freeb(bp);
		error(Ebadctl);
	}

	/* Only allow atomic Il writes to form datagrams */
	if(!(bp->flags & S_DELIM)) {
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
	ih->iltype = Ildata;
	ih->ilspec = 0;
	hnputl(ih->ilid, ic->sent++);
	hnputl(ih->ilack, ic->recvd);
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

	lock(ic);
	if(ic->unacked) {
		ic->unackedtail->next = np;
		ic->unackedtail = np;
	}
	else {
		ic->unacked = np;
		ic->unackedtail = np;
	}
	np->next = 0;
	unlock(ic);
}

void
ilackto(Ilcb *ic, ulong ackto)
{
	Ilhdr *h;
	Block *bp;

	for(;;) {
		lock(ic);
		if(ic->unacked) {
			h = (Ilhdr *)ic->unacked->rptr;
			if(ackto < nhgetl(h->ilack)) {
				unlock(ic);
				break;	
			}
			bp = ic->unacked;
			ic->unacked = bp->next;
			unlock(ic);
			freeb(bp);
		}
		else {
			unlock(ic);
			break;
		}
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
	int plen;
	Ipconv *s, *etab;
	short sp, dp;

	ih = (Ilhdr *)bp;

	plen = blen(bp);
	if(plen < IL_EHSIZE+IL_HDRSIZE)
		goto drop;

	if(ilcksum && ptcl_csum(bp, IL_EHSIZE, plen) != 0) {
		print("il: cksum error\n");
		goto drop;
	}

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	etab = &ipc[conf.ip];
	for(s = ipc; s < etab; s++) {
		if(s->psrc == sp && s->pdst == dp) {
			ilprocess(s, ih, bp);
			return;
		}
			
	}
	for(s = ipc; s < etab; s++) {
		if(s->ilctl.state == Illistening && s->psrc == 0) {
			/* Do the listener stuff */
			ilprocess(s, ih, bp);
			return;
		}
	}
	ilsendctl(0, ih, Ilreset, 0);
drop:
	freeb(bp);
}

void
ilprocess(Ipconv *s, Ilhdr *h, Block *bp)
{
	switch(s->ilctl.state) {
	case Ilclosed:
	case Ilclosing:
	case Illistening:
		error(Ehungup);
	}

	switch(h->iltype) {
	case Ilsync:
		if(s->ilctl.state == Ilsync)
			s->ilctl.state = Ilestablished;
		freeb(bp);
		break;
	case Ilack:
		ilackto(&s->ilctl, nhgetl(h->ilack));
		freeb(bp);
		break;
	case Ilquerey:
		ilsendctl(s, 0, Ilack, 1);
		freeb(bp);
		break;
	case Ildataquery:
	case Ildata:
		ilackto(&s->ilctl, nhgetl(h->ilack));
		bp->rptr += IL_EHSIZE+IL_HDRSIZE;
		PUTNEXT(s->readq, bp);
		break;
	case Ilreset:
		s->ilctl.state = Ilclosed;
		freeb(bp);
	}
}

void
ilsendctl(Ipconv *ipc, Ilhdr *inih, int type, int ack)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;

	bp = allocb(IL_EHSIZE+IL_HDRSIZE);
	bp->wptr += IL_EHSIZE+IL_HDRSIZE;

	ih = (Ilhdr *)(bp->rptr);
	ic = &ipc->ilctl;

	/* Ip fields */
	hnputl(ih->src, Myip);
	hnputl(ih->dst, ipc->dst);
	ih->proto = IP_ILPROTO;
	hnputs(ih->illen, IL_HDRSIZE);
	if(inih) {
		hnputs(ih->ilsrc, nhgets(inih->ildst));
		hnputs(ih->ildst, nhgets(inih->ilsrc));
		hnputl(ih->ilid, nhgetl(inih->ilack));
		hnputl(ih->ilack, nhgetl(inih->ilid));
	}
	else {
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

	if(!ack) {
		ic->sent++;			/* Maybe needs locking */
		ilackq(&ipc->ilctl, bp);
	}
	PUTNEXT(Ipoutput, bp);
}

void
ilackproc(void *junk)
{
}

void
ilstart(Ipconv *ipc, int type, int window)
{
	Ilcb *ic = &ipc->ilctl;

	if(ic->state != Ilclosed)
		return;

	ic->unacked = 0;
	ic->outoforder = 0;
	initseq += TK2MS(MACHP(0)->ticks);
	ic->sent = initseq;
	ic->recvd = ic->sent;
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
