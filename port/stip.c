/*
 *  ethernet specific multiplexor for ip
 *
 *  this line discipline gets pushed onto an ethernet channel
 *  to demultiplex/multiplex ip conversations.
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
int pip = 0;
int ipcksum = 1;
extern Ipifc *ipifc;
int Id = 1;

Fragq		*flisthead;
Fragq		*fragfree;
QLock		fraglock;

Queue 		*Etherq;

Ipaddr		Myip;
Ipaddr		Mymask;
Ipaddr		Mynetmask;
uchar		Netmyip[4];	/* In Network byte order */
uchar		bcast[4] = { 0xff, 0xff, 0xff, 0xff };

/* Predeclaration */
static void	ipetherclose(Queue*);
static void	ipetheriput(Queue*, Block*);
static void	ipetheropen(Queue*, Stream*);
static void	ipetheroput(Queue*, Block*);

/*
 *  the ethernet multiplexor stream module definition
 */
Qinfo ipinfo =
{
	ipetheriput,
	ipetheroput,
	ipetheropen,
	ipetherclose,
	"internet"
};

void
initfrag(int size)
{
	Fragq *fq, *eq;

	fragfree = (Fragq*)ialloc(sizeof(Fragq) * size, 0);

	eq = &fragfree[size];
	for(fq = fragfree; fq < eq; fq++)
		fq->next = fq+1;

	fragfree[size-1].next = 0;
}

/*
 *  set up an ether interface
 */
static void
ipetheropen(Queue *q, Stream *s)
{
	Ipconv *ipc;

	/* First open is by ipconfig and sets up channel
	 * to ethernet
	 */
	if(!Etherq) {
		Etherq = WR(q);
		s->opens++;		/* Hold this queue in place */
		s->inuse++;
	} else {
		ipc = &ipconv[s->dev][s->id];
		RD(q)->ptr = (void *)ipc;
		WR(q)->ptr = (void *)ipc;
		ipc->ref = 1;
	}

	DPRINT("ipetheropen EQ %lux dev=%d id=%d RD %lux WR %lux\n",
		Etherq, s->dev, s->id, RD(q), WR(q));
}

/*
 * newipifc - Attach to or Create a new protocol interface
 */

Ipifc *
newipifc(uchar ptcl, void (*recvfun)(Ipconv *, Block *bp),
	 Ipconv *con, int max, int min, int hdrsize, char *name)
{
	Ipifc *ifc, *free;
 
	free = 0;
	for(ifc = ipifc; ifc < &ipifc[conf.ipif]; ifc++) {
		qlock(ifc);
		if(ifc->protocol == ptcl) {
			ifc->ref++;
			qunlock(ifc);
			return(ifc);
		}
		if(!free && ifc->ref == 0) {
			ifc->ref = 1;
			free = ifc;
		}
		else
			qunlock(ifc);
	}

	if(!free)
		error(Enoifc);

	free->iprcv = recvfun;

	/* If media supports large transfer units limit maxmtu
	 * to max ip size */
	if(max > IP_MAX)
		max = IP_MAX;
	free->maxmtu = max;
	free->minmtu = min;
	free->hsize = hdrsize;
	free->connections = con;

	free->protocol = ptcl;
	strncpy(free->name, name, NAMELEN);

	qunlock(free);
	return(free);
}

static void
ipetherclose(Queue *q)
{
	Ipconv *ipc;

	ipc = (Ipconv *)(q->ptr);
	if(ipc){
		netdisown(ipc->net, ipc->index);
		ipc->ref = 0;
	}
}

void
closeipifc(Ipifc *ifc)
{
	/* If this is the last reference to the protocol multiplexor
	 * cancel upcalls from this stream
	 */
	qlock(ifc);
	if(--ifc->ref == 0) {
		ifc->protocol = 0;
		ifc->name[0] = 0;
	}
	qunlock(ifc);
}

static void
ipetheroput(Queue *q, Block *bp)
{
	Etherhdr *eh, *feh;
	int	 lid, len, seglen, chunk, dlen, blklen, offset;
	Ipifc	 *ifp;
	ushort	 fragoff;
	Block	 *xp, *nb;
	uchar 	 *ptr;

	if(bp->type != M_DATA){
		/* Allow one setting of the ip address */
		if(!Myip && streamparse("setip", bp)) {
			ptr = bp->rptr;
			Myip = ipparse((char *)ptr);
			Netmyip[0] = (Myip>>24)&0xff;
			Netmyip[1] = (Myip>>16)&0xff;
			Netmyip[2] = (Myip>>8)&0xff;
			Netmyip[3] = Myip&0xff;
			Mymask = classmask[Myip>>30];
			while(*ptr != ' ' && *ptr)
				ptr++;
			if(*ptr)
				Mymask = ipparse((char *)ptr);
			/*
			 * Temporary Until we understand real subnets
			 */
			Mynetmask = Mymask;
			freeb(bp);
		}
		else
			PUTNEXT(Etherq, bp);
		return;
	}

	ifp = (Ipifc *)(q->ptr);

	/* Number of bytes in ip and media header to write */
	len = blen(bp);

	/* Fill out the ip header */
	eh = (Etherhdr *)(bp->rptr);
	eh->vihl = IP_VER|IP_HLEN;
	eh->tos = 0;
	eh->ttl = 255;

	/* If we dont need to fragment just send it */
	if(len <= ifp->maxmtu) {
		hnputs(eh->length, len-ETHER_HDR);
		hnputs(eh->id, Id++);
		eh->frag[0] = 0;
		eh->frag[1] = 0;
		eh->cksum[0] = 0;
		eh->cksum[1] = 0;
		hnputs(eh->cksum, ip_csum(&eh->vihl));

		/* Finally put in the type and pass down to the arp layer */
		hnputs(eh->type, ET_IP);
		PUTNEXT(Etherq, bp);
		return;
	}

	if(eh->frag[0] & (IP_DF>>8))
		goto drop;

	seglen = (ifp->maxmtu - (ETHER_HDR+ETHER_IPHDR)) & ~7;
	if(seglen < 8)
		goto drop;

	/* Make prototype output header */
	hnputs(eh->type, ET_IP);
	
	dlen = len - (ETHER_HDR+ETHER_IPHDR);
	xp = bp;
	lid = Id++;

	offset = ETHER_HDR+ETHER_IPHDR;
	while(xp && offset && offset >= BLEN(xp)) {
		offset -= BLEN(xp);
		xp = xp->next;
	}
	xp->rptr += offset;

	for(fragoff = 0; fragoff < dlen; fragoff += seglen) {
		nb = allocb(ETHER_HDR+ETHER_IPHDR+seglen);
		feh = (Etherhdr *)(nb->rptr);

		memmove(nb->wptr, eh, ETHER_HDR+ETHER_IPHDR);
		nb->wptr += ETHER_HDR+ETHER_IPHDR;

		if((fragoff + seglen) >= dlen) {
			seglen = dlen - fragoff;
			hnputs(feh->frag, fragoff>>3);
		}
		else {	
			hnputs(feh->frag, (fragoff>>3)|IP_MF);
		}

		hnputs(feh->length, seglen + ETHER_IPHDR);
		hnputs(feh->id, lid);

		/* Copy up the data area */
		chunk = seglen;
		while(chunk) {
			if(!xp) {
				freeb(nb);
				goto drop;
			}
			blklen = MIN(BLEN(xp), chunk);
			memmove(nb->wptr, xp->rptr, blklen);
			nb->wptr += blklen;
			xp->rptr += blklen;
			chunk -= blklen;
			if(xp->rptr == xp->wptr)
				xp = xp->next;
		} 
				
		feh->cksum[0] = 0;
		feh->cksum[1] = 0;
		hnputs(feh->cksum, ip_csum(&feh->vihl));
		nb->flags |= S_DELIM;
		PUTNEXT(Etherq, nb);
	}
drop:
	freeb(bp);	
}


/*
 *  Input a packet and use the ip protocol to select the correct
 *  device to pass it to.
 *
 */
static void
ipetheriput(Queue *q, Block *bp)
{
	Ipifc 	 *ep, *ifp;
	Etherhdr *h;
	ushort   frag;

	if(bp->type != M_DATA){
		PUTNEXT(q, bp);
		return;
	}

	h = (Etherhdr *)(bp->rptr);

	/* Ensure we have enough data to process */
	if(BLEN(bp) < (ETHER_HDR+ETHER_IPHDR)) {
		bp = pullup(bp, ETHER_HDR+ETHER_IPHDR);
		if(bp == 0)
			return;
	}

	/* Look to see if its for me before we waste time checksuming it */
	if(ipforme(h->dst) == 0)
		goto drop;


	if(ipcksum && ip_csum(&h->vihl)) {
		print("ip: checksum error (from %d.%d.%d.%d ?)\n",
		      h->src[0], h->src[1], h->src[2], h->src[3]);
		goto drop;
	}

	/* Check header length and version */
	if(h->vihl != (IP_VER|IP_HLEN))
		goto drop;

	frag = nhgets(h->frag);
	if(frag) {
		h->tos = frag & IP_MF ? 1 : 0;
		bp = ip_reassemble(frag, bp, h);
		if(!bp)
			return;
	}

	/*
 	 * Look for an ip interface attached to this protocol
	 */
	ep = &ipifc[conf.ipif];
	for(ifp = ipifc; ifp < ep; ifp++)
		if(ifp->protocol == h->proto) {
			(*ifp->iprcv)(ifp->connections, bp);
			return;
		}
			
drop:
	freeb(bp);
}

int
ipforme(uchar *addr)
{
	Ipaddr haddr;

	if(memcmp(addr, Netmyip, sizeof(Netmyip)) == 0)
		return 1;

	haddr = nhgetl(addr);

	/* My subnet broadcast */
	if((haddr&Mymask) == (Myip&Mymask))
		return 1;

	/* My network broadcast */
	if((haddr&Mynetmask) == (Myip&Mynetmask))
		return 1;

	/* Real ip broadcast */
	if(haddr == 0)
		return 1;

	/* Old style 255.255.255.255 address */
	if(haddr == ~0)
		return 1;

	return 0;
}
Block *
ip_reassemble(int offset, Block *bp, Etherhdr *ip)
{
	Fragq *f;
	Ipaddr src, dst;
	ushort id;
	Block *bl, **l, *last, *prev;
	int ovlap, len, fragsize, pktposn;

	/* Check lance has handed us a contiguous buffer */
	if(bp->next)
		panic("ip: reass ?");

	src = nhgetl(ip->src);
	dst = nhgetl(ip->dst);
	id = nhgets(ip->id);

	qlock(&fraglock);
	for(f = flisthead; f; f = f->next) {
		if(f->src == src)
		if(f->dst == dst)
		if(f->id == id)
			break;
	}
	qunlock(&fraglock);

	if(!ip->tos && (offset & ~(IP_MF|IP_DF)) == 0) {
		if(f != 0) {
			qlock(f);
			ipfragfree(f, 1);
		}
		return(bp);
	}

	BLKFRAG(bp)->foff = offset<<3;
	BLKFRAG(bp)->flen = nhgets(ip->length) - ETHER_IPHDR; /* Ip data length */
	bp->flags &= ~S_DELIM;

	/* First fragment allocates a reassembly queue */
	if(f == 0) {
		f = ipfragallo();
		qlock(f);
		f->id = id;
		f->src = src;
		f->dst = dst;

		f->blist = bp;

		qunlock(f);
		return 0;
	}
	qlock(f);

	prev = 0;
	l = &f->blist;
	for(bl = f->blist; bl && BLKFRAG(bp)->foff > BLKFRAG(bl)->foff; bl = bl->next) {
		prev = bl;
		l = &bl->next;
	}

	/* Check overlap of a previous fragment - trim away as necessary */
	if(prev) {
		ovlap = BLKFRAG(prev)->foff + BLKFRAG(prev)->flen - BLKFRAG(bp)->foff;
		if(ovlap > 0) {
			if(ovlap > BLKFRAG(bp)->flen) {
				freeb(bp);
				qunlock(f);
				return 0;
			}
			BLKFRAG(bp)->flen -= ovlap;
		}
	}

	/* Link onto assembly queue */
	bp->next = *l;
	*l = bp;

	/* Check to see if suceeding segments overlap */
	if(bp->next) {
		l = &bp->next;
		end = BLKFRAG(bp)->foff + BLKFRAG(bp)->flen;
		/* Take completely covered segements out */
		while(*l && (ovlap = (end - BLKFRAG(*l)->foff)) > 0) {
			if(ovlap < BLKFRAG(*l)->flen) {
				BLKFRAG(*l)->flen -= ovlap;
				(*l)->rptr += ovlap;
				break;
			}	
			last = *l;
			freeb(*l);
			*l = last;
		}
	}

	pktposn = 0;
	for(bl = f->blist; bl; bl = bl->next) {
		if(BLKFRAG(bl)->foff != pktposn)
			break;
		if((BLKIP(bl)->frag[0]&(IP_MF>>8)) == 0)
			goto complete;

		pktposn += BLKFRAG(bl)->flen;
	}
	qunlock(f);
	return 0;

complete:
	bl = f->blist;
	last = bl;
	len = nhgets(BLKIP(bl)->length);
	bl->wptr = bl->rptr + len + ETHER_HDR;

	/* Pullup all the fragment headers and return a complete packet */
	for(bl = bl->next; bl; bl = bl->next) {
		fragsize = BLKFRAG(bl)->flen;
		len += fragsize;
		bl->rptr += (ETHER_HDR+ETHER_IPHDR);
		bl->wptr = bl->rptr + fragsize;
		last = bl;
	}

	last->flags |= S_DELIM;
	bl = f->blist;
	f->blist = 0;
	ipfragfree(f, 1);

	ip = BLKIP(bl);
	hnputs(ip->length, len);

	return(bl);		
}

/*
 * ipfragfree - Free a list of fragments, fragment list must be locked
 */

void
ipfragfree(Fragq *frag, int lockq)
{
	Fragq *fl, **l;

	if(frag->blist)
		freeb(frag->blist);

	frag->src = 0;
	frag->id = 0;
	frag->blist = 0;
	qunlock(frag);

	if(lockq)
		qlock(&fraglock);

	l = &flisthead;
	for(fl = *l; fl; fl = fl->next) {
		if(fl == frag) {
			*l = frag->next;
			break;
		}
		l = &fl->next;
	}

	frag->next = fragfree;
	fragfree = frag;

	if(lockq)
		qunlock(&fraglock);
}

/*
 * ipfragallo - allocate a reassembly queue
 */
Fragq *
ipfragallo(void)
{
	Fragq *f;

	qlock(&fraglock);
	while(fragfree == 0) {
		for(f = flisthead; f; f = f->next)
			if(canqlock(f)) {
				ipfragfree(f, 0);
				break;
			}
	}
	f = fragfree;
	fragfree = f->next;
	f->next = flisthead;
	flisthead = f;
	f->age = TK2MS(MACHP(0)->ticks) + 30000;

	qunlock(&fraglock);
	return f;
}

/*
 * ip_csum - Compute internet header checksums
 */
ushort
ip_csum(uchar *addr)
{
	int len;
	ulong sum = 0;

	len = (addr[0]&0xf)<<2;

	while(len > 0) {
		sum += addr[0]<<8 | addr[1] ;
		len -= 2;
		addr += 2;
	}

	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (sum^0xffff);
}

/*
 * ipparse - Parse an ip address out of a string
 */

Ipaddr classmask[4] = {
	0xff000000,
	0xff000000,
	0xffff0000,
	0xffffff00
};

Ipaddr
ipparse(char *ipa)
{
	Ipaddr address = 0;
	int shift;
	Ipaddr net;

	shift = 24;

	while(shift >= 0 && ipa != (char *)1) {
		address |= atoi(ipa) << shift;
		shift -= 8;
		ipa = strchr(ipa, '.')+1;
	}
	net = address & classmask[address>>30];

	shift += 8;
	return net | ((address & ~classmask[address>>30])>>shift);
}
