#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/netif.h"

enum {
	Ntypes=		9,	/* max number of ethernet packet types */
	Maxrb=		128,	/* max buffers in a ring */
};
#define RSUCC(x) (((x)+1)%l.nrrb)
#define TSUCC(x) (((x)+1)%l.ntrb)

/*
 *  Communication with the lance is via a transmit and receive ring of
 *  message descriptors.  The Initblock contains pointers to and sizes of
 *  these rings.  The rings must be in RAM addressible by the lance
 */
typedef struct {
	ushort	laddr;		/* low order piece of address */
	ushort	flags;		/* flags and high order piece of address */
	short	size;		/* size of buffer */
	ushort	cntflags;	/* (rcv)count of buffer; (xmt) more flags */
} Msg;

/*
 *  lance memory map
 */
struct Lancemem
{
	/*
	 *  initialization block
	 */
	ushort	mode;		/* chip control (see below) */
	ushort	etheraddr[3];	/* the ethernet physical address */
	ushort	multi[4];	/* multicast addresses, 1 bit for each of 64 */
	ushort	rdralow;	/* receive buffer ring */
	ushort	rdrahigh;	/* (top three bits define size of ring) */
	ushort	tdralow;	/* transmit buffer ring */
	ushort	tdrahigh;	/* (top three bits define size of ring) */
	
	/*
	 *  ring buffers
	 *  first receive, then transmit
	 */
	Msg	rmr[Maxrb];		/* recieve message ring */
	Msg	tmr[Maxrb];		/* transmit message ring */
};

/*
 *  Some macros for dealing with lance memory addresses.  The lance splits
 *  its 24 bit addresses across two 16 bit registers.
 */
#define HADDR(a) ((((ulong)(a))>>16)&0xFF)
#define LADDR(a) (((ulong)a)&0xFFFF)

/*
 *  The following functions exist to sidestep a quirk in the SGI IO3 lance
 *  interface.  In all other processors, the lance's initialization block and
 *  descriptor rings look like normal memory.  In the SGI IO3, the CPU sees a
 *  6 byte pad twixt all lance memory shorts.  Therefore, we use the following
 *  macros to compute the address whenever accessing the lance memory to make
 *  the code portable.  Sic transit gloria.
 */
#define LANCEMEM ((Lancemem*)0)
#define MPs(a) (*(short *)(l.lanceram + l.sep*((ushort*)&a - (ushort*)0)))
#define MPus(a) (*(ushort *)(l.lanceram + l.sep*((ushort*)&a - (ushort*)0)))

enum
{
	PROM	= 0x8000,
	INTL	= 0x40,
	DRTY	= 0x20,
	COLL	= 0x10,
	DTCR	= 0x8,
	LOOP	= 0x4,
	DTX	= 0x2,
	DRX	= 0x1,
	ERR0	= 0x8000,
	BABL	= 0x4000,
	CERR	= 0x2000,
	MISS	= 0x1000,
	MERR	= 0x800,
	RINT	= 0x400,
	TINT	= 0x200,
	IDON	= 0x100,
	INTR	= 0x80,
	INEA	= 0x40,
	RXON	= 0x20,
	TXON	= 0x10,
	TDMD	= 0x8,
	STOP	= 0x4,
	STRT	= 0x2,
	INIT	= 0x1,
	/* flag bits from a buffer descriptor in the rcv/xmt rings */
	LANCEOWNER= 0x8000,	/* 1 means buffer can be used by the chip */
	ERR	= 0x4000,	/* error summary, the OR of all error bits */
	FRAM	= 0x2000,	/* CRC error */
	OFLO	= 0x1000,	/* (receive) lost some of the packet */
	MORE	= 0x1000,	/* (transmit) more than 1 retry to tx pkt */
	CRC	= 0x800,	/* (rcv) crc error reading packet */
	ONE	= 0x800,	/* (tx) one retry to transmit the packet */
	BUF	= 0x400,	/* (rcv) out of buffers */
	DEF	= 0x400,	/* (tx) deffered while transmitting packet */
	STP	= 0x200,	/* start of packet */
	ENP	= 0x100,	/* end of packet */
	/* cntflags bits from a buffer descriptor in the rcv/xmt rings */
	BUFF	= 0x8000,	/* buffer error (host screwed up?) */
	UFLO	= 0x4000,	/* underflow from memory */
	LCOL	= 0x1000,	/* late collision (ether too long?) */
	LCAR	= 0x800,	/* loss of carrier (ether broken?) */
	RTRY	= 0x400,	/* couldn't transmit (ether jammed?) */
	TTDR	= 0x3FF,	/* time domain reflectometer */
};

struct Softlance
{
	Lance;			/* host dependent lance params */

	uchar	bcast[6];

	int	wedged;		/* the lance is wedged */

	ushort	rl;		/* first rcv Message belonging to Lance */	
	ushort	tc;		/* next xmt Message CPU will try for */	

	QLock	tlock;		/* lock for grabbing transmitter queue */
	Rendez	tr;		/* wait here for free xmit buffer */

	Netif;
} l;

static void	promiscuous(void*, int);

void
lancereset(void)
{
	static int inited;

	if(inited == 0){
		inited = 1;
		lancesetup(&l);

		/* general network interface structure */
		netifinit(&l, "ether", Ntypes, 32*1024);
		l.alen = 6;
		memmove(l.addr, l.ea, 6);
		memmove(l.bcast, etherbcast, 6);

		l.promiscuous = promiscuous;
		l.arg = &l;
	}

	/*
	 *  stop the lance
	 */
	*l.rap = 0;
	*l.rdp = STOP;
	l.wedged = 1;
}

/*
 * Initialize and start the lance. 
 * This routine can be called only from a process.
 * It may be used to restart a dead lance.
 */
static void
lancestart(int mode)
{
	int i;
	Msg *m;
	Lancemem *lm = LANCEMEM;

	/*
	 *   wait till both receiver and transmitter are
	 *   quiescent
	 */
	qlock(&l.tlock);

	lancereset();
	l.rl = 0;
	l.tc = 0;

	/*
	 *  create the initialization block
	 */
	MPus(lm->mode) = mode;

	/*
	 *  set ether addr from the value in the id prom.
	 *  the id prom has them in reverse order, the init
	 *  structure wants them in byte swapped order
	 */
	MPus(lm->etheraddr[0]) = (l.ea[1]<<8) | l.ea[0];
	MPus(lm->etheraddr[1]) = (l.ea[3]<<8) | l.ea[2];
	MPus(lm->etheraddr[2]) = (l.ea[5]<<8) | l.ea[4];

	/*
	 *  ignore multicast addresses
	 */
	MPus(lm->multi[0]) = 0;
	MPus(lm->multi[1]) = 0;
	MPus(lm->multi[2]) = 0;
	MPus(lm->multi[3]) = 0;

	/*
	 *  set up rcv message ring
	 */
	m = lm->rmr;
	for(i = 0; i < l.nrrb; i++, m++){
		MPs(m->size) = -sizeof(Lancepkt);
		MPus(m->cntflags) = 0;
		MPus(m->laddr) = LADDR(&l.lrp[i]);
		MPus(m->flags) = HADDR(&l.lrp[i]);
	}
	MPus(lm->rdralow) = LADDR(l.lm->rmr);
	MPus(lm->rdrahigh) = (l.lognrrb<<13)|HADDR(l.lm->rmr);


	/*
	 *  give the lance all the rcv buffers except one (as a sentinel)
	 */
	m = lm->rmr;
	for(i = 0; i < l.nrrb; i++, m++)
		MPus(m->flags) |= LANCEOWNER;

	/*
	 *  set up xmit message ring
	 */
	m = lm->tmr;
	for(i = 0; i < l.ntrb; i++, m++){
		MPs(m->size) = 0;
		MPus(m->cntflags) = 0;
		MPus(m->laddr) = LADDR(&l.ltp[i]);
		MPus(m->flags) = HADDR(&l.ltp[i]);
	}
	MPus(lm->tdralow) = LADDR(l.lm->tmr);
	MPus(lm->tdrahigh) = (l.logntrb<<13)|HADDR(l.lm->tmr);

	/*
	 *  point lance to the initialization block
	 */
	*l.rap = 1;
	*l.rdp = LADDR(l.lm);
	wbflush();
	*l.rap = 2;
	*l.rdp = HADDR(l.lm);

	/*
	 *  The lance byte swaps the ethernet packet unless we tell it not to
	 */
	wbflush();
	*l.rap = 3;
	*l.rdp = l.busctl;

	/*
	 *  initialize lance, turn on interrupts, turn on transmit and rcv.
	 */
	wbflush();
	*l.rap = 0;
	*l.rdp = INEA|INIT|STRT; /**/

	for(i = 0; i < 1000; i++)
		if(l.wedged == 0)
			break;

	qunlock(&l.tlock);
}

void
lanceintr(void)
{
	Msg *m;
	Lancepkt *p;
	Netfile *f, **fp;
	ushort csr, t, x;
	Lancemem *lm = LANCEMEM;

	csr = *l.rdp;

	/*
	 *  turn off the interrupt and any error indicators
	 */
	*l.rdp = IDON|INEA|TINT|RINT|BABL|CERR|MISS|MERR;

	/*
	 *  see if an error occurred
	 */
	if(csr & (BABL|MISS|MERR)){
		if(l.misses++ < 4)
			print("lance err #%ux\n", csr);
		else {
			print("lance stopped\n");
			l.wedged = 1;
			l.misses = 0;
			lancereset();
			wakeup(&l.tr);
			return;
		}
	}

	if(csr & IDON)
		l.wedged = 0;

	/*
	 *  the lance turns off if it gets strange output errors
	 */
	if((csr & (TXON|RXON)) != (TXON|RXON))
		l.wedged = 1;

	/*
	 *  process incoming frames
	 */
	if(csr & RINT) {
		m = &(lm->rmr[l.rl]);
		while((MPus(m->flags) & LANCEOWNER)==0){
			l.inpackets++;
			t = MPus(m->flags);
			if(t & ERR){
				if(t & FRAM)
					l.frames++;
				if(t & OFLO)
					l.overflows++;
				if(t & CRC)
					l.crcs++;
				if(t & BUFF)
					l.buffs++;
			} else {
				/* stuff packet up each queue that wants it */
				p = &l.rp[l.rl];
				x = MPus(m->cntflags) - 4;
				t = (p->type[0]<<8) | p->type[1];

				for(fp = l.f; fp < &l.f[Ntypes]; fp++){
					f = *fp;
					if(f == 0)
						continue;
					if(f->type == t || f->type < 0)
						qproduce(f->in, p->d, x);
				}
			}

			/*
			 *  stage the next input buffer
			 */
			MPs(m->size) = -sizeof(Lancepkt);
			MPus(m->cntflags) = 0;
			MPus(m->laddr) = LADDR(&l.lrp[l.rl]);
			MPus(m->flags) = LANCEOWNER|HADDR(&l.lrp[l.rl]);
			wbflush();
			l.rl = RSUCC(l.rl);
			m = &(lm->rmr[l.rl]);
		}
	}

	/*
	 *  wake any process waiting for a transmit buffer
	 */
	if(csr & TINT)
		wakeup(&l.tr);
}

/*
 *  turn promiscuous mode on/off
 */
static void
promiscuous(void *arg, int on)
{
	USED(arg);
	if(on)
		lancestart(PROM);
	else
		lancestart(0);;
}

void
lanceinit(void)
{
	lancestart(0);
	print("lance ether: %.2x%.2x%.2x%.2x%.2x%.2x\n",
		l.ea[0], l.ea[1], l.ea[2], l.ea[3], l.ea[4], l.ea[5]);
}

Chan*
lanceattach(char *spec)
{
	return devattach('l', spec);
}

Chan*
lanceclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
lancewalk(Chan *c, char *name)
{
	return netifwalk(&l, c, name);
}

Chan*
lanceopen(Chan *c, int omode)
{
	return netifopen(&l, c, omode);
}

void
lancecreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
}

void
lanceclose(Chan *c)
{
	netifclose(&l, c);
}

long
lanceread(Chan *c, void *buf, long n, ulong offset)
{
	return netifread(&l, c, buf, n, offset);
}

Block*
lancebread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static int
isoutbuf(void *x)
{
	Msg *m;

	m = x;
	return l.wedged || (MPus(m->flags)&LANCEOWNER) == 0;
}

static int
etherloop(Etherpkt *p, long n)
{
	int s, different;
	ushort t;
	Netfile *f, **fp;

	different = memcmp(p->d, p->s, sizeof(p->d));
	if(different && memcmp(p->d, l.bcast, sizeof(p->d)))
		return 0;

	s = splhi();
	t = (p->type[0]<<8) | p->type[1];
	for(fp = l.f; fp < &l.f[Ntypes]; fp++) {
		f = *fp;
		if(f == 0)
			continue;
		if(f->type == t || f->type < 0)
			qproduce(f->in, p->d, n);
	}
	splx(s);
	return !different;
}

long
lancewrite(Chan *c, void *buf, long n, ulong offset)
{
	Msg *m;
	Lancepkt *p;

	USED(offset);

	if(n > ETHERMAXTU)
		error(Ebadarg);

	/* etherif.c handles structure */
	if(NETTYPE(c->qid.path) != Ndataqid)
		return netifwrite(&l, c, buf, n);

	/* we handle data */
	if(etherloop(buf, n))
		return n;

	qlock(&l.tlock);
	if(waserror()) {
		qunlock(&l.tlock);
		nexterror();
	}

	m = &(LANCEMEM->tmr[l.tc]);
	tsleep(&l.tr, isoutbuf, m, 10000);
	if(!isoutbuf(m) || l.wedged)
		print("lance transmitter jammed\n");
	else {
		p = &l.tp[l.tc];
		memmove(p->d, buf, n);
		if(n < 60) {
			memset(p->d+n, 0, 60-n);
			n = 60;
		}
		memmove(p->s, l.ea, sizeof(l.ea));

		l.outpackets++;
		MPs(m->size) = -n;
		MPus(m->cntflags) = 0;
		MPus(m->laddr) = LADDR(&l.ltp[l.tc]);
		MPus(m->flags) = LANCEOWNER|STP|ENP|HADDR(&l.ltp[l.tc]);
		l.tc = TSUCC(l.tc);
		*l.rdp = INEA|TDMD;
	}
	qunlock(&l.tlock);
	poperror();
	return n;
}

long
lancebwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void
lanceremove(Chan *c)
{
	USED(c);
}

void
lancestat(Chan *c, char *dp)
{
	netifstat(&l, c, dp);
}

void
lancewstat(Chan *c, char *dp)
{
	netifwstat(&l, c, dp);
}
