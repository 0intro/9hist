/*
 *  ethernet specific multiplexor for nonet
 *
 *  this line discipline gets pushed onto an ethernet channel
 *  to demultiplex/multiplex nonet conversations.
 */
#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"../port/nonet.h"

#define DPRINT if(pnonet)print
extern int pnonet;

static void	etherparse(uchar*, char*);
static void	noetherclose(Queue*);
static void	noetheriput(Queue*, Block*);
static void	noetheropen(Queue*, Stream*);
static void	noetheroput(Queue*, Block*);

/*
 *  ethernet header of a packet
 */
typedef struct Etherhdr	Etherhdr;

struct Etherhdr {
	uchar	d[6];
	uchar	s[6];
	uchar	type[2];
	uchar	circuit[3];	/* circuit number */
	uchar	flag;
	uchar	mid;		/* message id */
	uchar	ack;		/* piggy back ack */
	uchar	remain[2];	/* count of remaing bytes of data */
	uchar	sum[2];		/* checksum (0 means none) */
};
#define EHDRSIZE 24
#define EMAXBODY	(1514-HDRSIZE)	/* maximum ethernet packet body */
#define ETHER_TYPE	0x900	/* most significant byte last */

/*
 *  the ethernet multiplexor stream module definition
 */
Qinfo noetherinfo =
{
	noetheriput,
	noetheroput,
	noetheropen,
	noetherclose,
	"noether"
};

/*
 *  perform the ether specific part of nonetconnect.  just stick
 *  the address into the prototype header.
 */
void
noetherconnect(Noconv *cp, char *ea)
{
	Etherhdr *eh;

	eh = (Etherhdr *)cp->media->rptr;
	etherparse(eh->d, ea);
	eh->type[0] = ETHER_TYPE>>8;
	eh->type[1] = ETHER_TYPE & 0xff;
}

/*
 *  set up an ether interface
 */
static void
noetheropen(Queue *q, Stream *s)
{
	nonetnewifc(q, s, 1514, ETHERMINMTU, 14, noetherconnect);
}

/*
 *  tear down an ether interface
 */
static void
noetherclose(Queue *q)
{
	Noifc *ifc;

	ifc = (Noifc *)(q->ptr);
	nonetfreeifc(ifc);
}

/*
 *  configure the system
 */
static void
noetheroput(Queue *q, Block *bp)
{
	Noifc *ifc;

	ifc = (Noifc *)(q->ptr);
	if(bp->type != M_DATA){
		if(streamparse("config", bp)){
			if(*bp->rptr == 0)
				strcpy(ifc->name, "nonet");
			else
				strncpy(ifc->name, (char *)bp->rptr, sizeof(ifc->name));
		} else
			PUTNEXT(q, bp);
		return;
	}

	PUTNEXT(q, bp);
}

/*
 *  respond to a misaddressed message with a close
 */
void
noetherbad(Noifc *ifc, Block *bp, int circuit)
{
	Etherhdr *eh, *neh;
	Block *nbp;
	int r;
	Noconv *cp, *ep;

	/*
	 *  crack the packet header
	 */
	eh = (Etherhdr *)bp->rptr;
	print("bad %.2ux%.2ux%.2ux%.2ux%.2ux%.2ux c %d m %d f %d\n",
		eh->s[0], eh->s[1], eh->s[2], eh->s[3], eh->s[4],
		eh->s[5], circuit, eh->mid, eh->flag);
	if(eh->flag & NO_RESET)
		goto out;

ep = &ifc->conv[conf.nnoconv];
for(cp = &ifc->conv[0]; cp < ep; cp++){
	qlock(cp);
	if(cp->media){
		neh = (Etherhdr *)(cp->media->rptr);
		print("%lux	%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux %s c %d\n", neh,
			neh->s[0], neh->s[1], neh->s[2], neh->s[3], neh->s[4],
			neh->s[5], cp->raddr, cp->rcvcircuit);
	}
	qunlock(cp);
}

	/*
	 *  only one reset per message
	 */
	r = (eh->remain[1]<<8) | eh->remain[0];
	if(r<0)
		goto out;

	/*
	 *  craft an error reply
	 */
	print("sending reset\n");
	nbp = allocb(60);
	nbp->flags |= S_DELIM;
	nbp->wptr = nbp->rptr + 60;
	memset(bp->rptr, 0, 60);
	neh = (Etherhdr *)nbp->rptr;
	memmove(neh, eh, sizeof(Etherhdr));
	neh->circuit[0] ^= 1;
	neh->remain[0] = neh->remain[1] = 0;
	neh->flag = NO_HANGUP | NO_RESET;
	neh->ack = eh->mid;
	neh->mid = eh->ack;
	memmove(neh->s, eh->d, sizeof(neh->s));
	memmove(neh->d, eh->s, sizeof(neh->d));
	nonetcksum(nbp, 14);
	PUTNEXT(ifc->wq, nbp);
out:
	freeb(bp);
}

/*
 *  Input a packet and use the ether address to select the correct
 *  nonet device to pass it to.
 *
 *  Simplifying assumption:  one put == one packet && the complete header
 *	is in the first block.  If this isn't true, demultiplexing will not work.
 */
static void
noetheriput(Queue *q, Block *bp)
{
	Noifc *ifc;
	int circuit;
	Noconv *cp, *ep;
	Etherhdr *h;
	Etherhdr *ph;
	ulong s;
	Block *nbp;
	int next;
	Nocall *clp;

	if(bp->type != M_DATA){
		PUTNEXT(q, bp);
		return;
	}

	ifc = (Noifc *)(q->ptr);
	h = (Etherhdr *)(bp->rptr);
	circuit = (h->circuit[2]<<16) | (h->circuit[1]<<8) | h->circuit[0];
	s = (h->sum[1]<<8) | h->sum[0];
	if(s && s!=nonetcksum(bp, 14)){
		print("checksum error %ux %ux\n", s, (h->sum[1]<<8) | h->sum[0]); /**/
		freeb(bp);
		return;
	}

	/*
	 *  look for an existing circuit.
	 */
	ep = &ifc->conv[conf.nnoconv];
	for(cp = &ifc->conv[0]; cp < ep; cp++){
		if(circuit==cp->rcvcircuit){
			qlock(cp);
			ph = (Etherhdr *)(cp->media->rptr);
			if(circuit == cp->rcvcircuit
			&& memcmp(ph->d, h->s, sizeof(h->s)) == 0){
				bp->rptr += ifc->hsize;
				nonetrcvmsg(cp, bp);
				qunlock(cp);
				return;
			}
			qunlock(cp);
		}
	}

	/*
	 *  if not a new call, then its misaddressed
	 */
	if((h->flag & NO_NEWCALL) == 0){
		noetherbad(ifc, bp, circuit);
		return;
	}

	/*
	 *  Queue call in a circular queue and wakeup a listener.
	 */
	DPRINT("call in\n");
	lock(&ifc->lock);
	next = (ifc->wptr + 1) % Nnocalls;
	if(next == ifc->rptr){
		/* no room in the queue */
		unlock(&ifc->lock);
		freeb(bp);
		return;
	}
	clp = &ifc->call[ifc->wptr];
	sprint(clp->raddr, "%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux",
		h->s[0], h->s[1], h->s[2], h->s[3], h->s[4], h->s[5]);
	clp->circuit = circuit^1;
	bp->rptr += ifc->hsize;
	clp->msg = bp;
	ifc->wptr = next;
	unlock(&ifc->lock);
	wakeup(&ifc->listenr);
}

/*
 *  parse an ethernet address (assumed to be 12 ascii hex digits)
 */
static void
etherparse(uchar *to, char *from)
{
	int tdig;
	int fdig;
	int i;

	if(strlen(from) != 12)
		error(Ebadnet);

	for(i = 0; i < 6; i++){
		fdig = *from++;
		tdig = fdig > 'a' ? (fdig - 'a' + 10)
				: (fdig > 'A' ? (fdig - 'A' + 10) : (fdig - '0'));
		fdig = *from++;
		tdig <<= 4;
		tdig |= fdig > 'a' ? (fdig - 'a' + 10)
				: (fdig > 'A' ? (fdig - 'A' + 10) : (fdig - '0'));
		*to++ = tdig;
	}
}
