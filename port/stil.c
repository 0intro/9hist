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
Queue	*Iloutput;		/* Il to lance output channel */

void	ilrcvmsg(Ipconv*, Block*);
void	ilackproc(void*);

void
ilopen(Queue *q, Stream *s)
{
	Ipconv *ipc;

	/* Start il service processes */
	if(!Iloutput) {
		Iloutput = WR(q);
		/* This never goes away - we use this queue to send acks/rejects */
		s->opens++;
		s->inuse++;
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

	/* Prepend udp header to packet and pass on to ip layer */
	ipc = (Ipconv *)(q->ptr);
	if(ipc->psrc == 0)
		error(Enoport);

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

	hnputs(ih->illen, dlen+IL_EHSIZE+IL_HDRSIZE);
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

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyb(bp, blen(bp));
	if(ic->unacked) {
		ic->unackedtail->next = np;
		ic->unackedtail = np;
	}
	else {
		ic->unacked = np;
		ic->unackedtail = np;
	}
	np->next = 0;

	PUTNEXT(q, bp);
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

	ih = (Ilhdr *)bp;

	plen = blen(bp);
	if(plen < IL_EHSIZE+IL_HDRSIZE)
		goto drop;

	if(ilcksum && ptcl_csum(bp, IL_EHSIZE, plen) != 0) {
		print("il: cksum error\n");
		goto drop;
	}

	s = ip_conn(ipc, hngets(ih->ildst), hngets(ih->ilsrc), hngetl(), IP_ILPROTO);
	if(s == 0) {
		ilsentctl(0, ih, Ilreset);
		goto drop;
	}

	switch(s->state) {
	case Closed:
		ilsentctl(0, ih, Ilreset);
		goto drop;
	case Syncee:
	case Syncer:
	case Established:
	case Closing:
	}	
	
drop:
	freeb(bp);
}

void
ilsendctl(Ipconv *ipc, Ilhdr *inih, int type)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;

	bp = allocb(IL_EHSIZE+IL_HDRSIZE);
	ih = (Ilhdr *)(bp->rptr);
	ic = &ipc->ilctl;

	hnputs(ih->illen, IL_EHSIZE+IL_HDRSIZE);
	if(inih) {
		hnputs(ih->ilsrc, inih->ildst);
		hnputs(ih->ildst, inih->ilsrc);
		hnputl(ih->ilid, inih->recvd);
		hnputl(ih->ilack, inih->sent);
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

	PUTNEXT(Iloutput, bp);
}

void
ilackproc(void *junk)
{
}
