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
#define EHDRSIZE 	(ETHERHDRSIZE + NO_HDRSIZE)
#define EMAXBODY	(ETHERMAXTU - EHDRSIZE)	/* maximum ethernet packet body */
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
	Etherpkt *eh;

	eh = (Etherpkt*)cp->media->rptr;
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
	streamenter(s);
	nonetnewifc(q, s, ETHERMAXTU, ETHERMINTU, ETHERHDRSIZE, noetherconnect);
}

/*
 *  tear down an ether interface
 */
static void
noetherclose(Queue *q)
{
	Noifc *ifc;

	ifc = (Noifc*)q->ptr;
	nonetfreeifc(ifc);
}

/*
 *  configure the system
 */
static void
noetheroput(Queue *q, Block *bp)
{
	Noifc *ifc;

	ifc = (Noifc*)q->ptr;
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
	Etherpkt *eh, *neh;
	Nohdr *nh, *nnh;
	Block *nbp;
	int r;
	Noconv *cp, *ep;

	/*
	 *  crack the packet header
	 */
	eh = (Etherpkt*)bp->rptr;
	nh = (Nohdr*)eh->data;
/*	print("bad %.2ux%.2ux%.2ux%.2ux%.2ux%.2ux c %d m %d f %d\n",
		eh->s[0], eh->s[1], eh->s[2], eh->s[3], eh->s[4],
		eh->s[5], circuit, nh->mid, nh->flag); /**/
	if(nh->flag & NO_RESET)
		goto out;

	/*
	 *  only one reset per message
	 */
	r = (nh->remain[1]<<8) | nh->remain[0];
	if(r<0)
		goto out;

	/*
	 *  craft an error reply
	 */
/*	print("sending reset\n"); /**/
	nbp = allocb(60);
	nbp->flags |= S_DELIM;
	nbp->wptr = nbp->rptr + 60;
	memset(bp->rptr, 0, 60);
	neh = (Etherpkt *)nbp->rptr;
	nnh = (Nohdr*)neh->data;
	memmove(neh, eh, EHDRSIZE);
	nnh->circuit[0] ^= 1;
	nnh->remain[0] = nnh->remain[1] = 0;
	nnh->flag = NO_HANGUP | NO_RESET;
	nnh->ack = nh->mid;
	nnh->mid = nh->ack;
	memmove(neh->s, eh->d, sizeof(neh->s));
	memmove(neh->d, eh->s, sizeof(neh->d));
	nonetcksum(nbp, ETHERHDRSIZE);
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
	Etherpkt *eh, *peh;
	Nohdr *nh;
	ulong s;
	Block *nbp;
	int next;
	Nocall *clp;

	if(bp->type != M_DATA){
		PUTNEXT(q, bp);
		return;
	}

	ifc = (Noifc*)q->ptr;
	eh = (Etherpkt*)bp->rptr;
	nh = (Nohdr*)eh->data;
	circuit = (nh->circuit[2]<<16) | (nh->circuit[1]<<8) | nh->circuit[0];
	s = (nh->sum[1]<<8) | nh->sum[0];
	if(s && s!=nonetcksum(bp, ETHERHDRSIZE)){
/*		print("checksum error %ux %ux\n", s, (nh->sum[1]<<8) | nh->sum[0]); /**/
		freeb(bp);
		return;
	}

	/*
	 *  look for an existing circuit.
	 */
	ep = &ifc->conv[conf.nnoconv];
	for(cp = &ifc->conv[0]; cp < ep; cp++){
		nbp = cp->media;
		if(nbp == 0)
			continue;
		peh = (Etherpkt*)nbp->rptr;
		if(circuit==cp->rcvcircuit && memcmp(peh->d, eh->s, sizeof(eh->s))==0){
			if(!canqlock(cp)){
				freeb(bp);
				return;
			}
			peh = (Etherpkt*)nbp->rptr;
			if(circuit==cp->rcvcircuit
			&& memcmp(peh->d, eh->s, sizeof(eh->s))==0){
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
	if((nh->flag & NO_NEWCALL) == 0){
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
		eh->s[0], eh->s[1], eh->s[2], eh->s[3], eh->s[4], eh->s[5]);
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
	ulong ip;
	uchar nip[4];
	int tdig;
	int fdig;
	int i;

#ifdef MAGNUM
	/* Dot means ip address */
	if(strchr(from, '.')) {
		ip = ipparse(from);
		if(ip == 0)
			error(Ebadnet);

		hnputl(nip, ip);			
		if(arp_lookup(nip, to))
			return;
	}
#endif

	if(strlen(from) != 12)
		error(Ebadnet);

	for(i = 0; i < 6; i++){
		fdig = (*from++)&0x7f;
		tdig = fdig >= 'a' ? ((fdig - 'a') + 10)
				: (fdig >= 'A' ? ((fdig - 'A') + 10) : (fdig - '0'));
		fdig = (*from++)&0x7f;
		tdig <<= 4;
		tdig |= fdig >= 'a' ? ((fdig - 'a') + 10)
				: (fdig >= 'A' ? ((fdig - 'A') + 10) : (fdig - '0'));
		*to++ = tdig;
	}
}
