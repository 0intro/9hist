#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"
#include	"../port/netif.h"

/*
 * National Semiconductor DP83932
 * Systems-Oriented Network Interface Controller
 * (SONIC)
 */

#define SONICADDR	((Sonic*)Sonicbase)

#define RD(rn)		(delay(1), *(ulong*)((ulong)&SONICADDR->rn^4))
#define WR(rn, v)	(delay(1), *(ulong*)((ulong)&SONICADDR->rn^4) = v)

typedef struct
{
	ulong	cr;		/* command */
	ulong	dcr;		/* data configuration */
	ulong	rcr;		/* receive control */
	ulong	tcr;		/* transmit control */
	ulong	imr;		/* interrupt mask */
	ulong	isr;		/* interrupt status */
	ulong	utda;		/* upper transmit descriptor address */
	ulong	ctda;		/* current transmit descriptor address */
	ulong	pad0x08[5];	/*  */
	ulong	urda;		/* upper receive descriptor address */
	ulong	crda;		/* current receive descriptor address */
	ulong	pad0x0F[4];	/*  */
	ulong	eobc;		/* end of buffer word count */
	ulong	urra;		/* upper receive resource address */
	ulong	rsa;		/* resource start address */
	ulong	rea;		/* resource end address */
	ulong	rrp;		/* resource read pointer */
	ulong	rwp;		/* resource write pointer */
	ulong	pad0x19[8];	/*  */
	ulong	cep;		/* CAM entry pointer */
	ulong	cap2;		/* CAM address port 2 */
	ulong	cap1;		/* CAM address port 1 */
	ulong	cap0;		/* CAM address port 0 */
	ulong	ce;		/* CAM enable */
	ulong	cdp;		/* CAM descriptor pointer */
	ulong	cdc;		/* CAM descriptor count */
	ulong	sr;		/* silicon revision */
	ulong	wt0;		/* watchdog timer 0 */
	ulong	wt1;		/* watchdog timer 1 */
	ulong	rsc;		/* receive sequence counter */
	ulong	crct;		/* CRC error tally */
	ulong	faet;		/* FAE tally */
	ulong	mpt;		/* missed packet tally */
	ulong	mdt;		/* maximum deferral timer */
	ulong	pad0x30[15];	/*  */
	ulong	dcr2;		/* data configuration 2 */
} Sonic;

enum
{
	Nrb		= 16,		/* receive buffers */
	Ntb		= 8,		/* transmit buffers */
};

enum
{
	Htx	= 0x0001,	/* halt transmission */
	Txp	= 0x0002,	/* transmit packet(s) */
	Rxdis	= 0x0004,	/* receiver disable */
	Rxen	= 0x0008,	/* receiver enable */
	Stp	= 0x0010,	/* stop timer */
	St	= 0x0020,	/* start timer */
	Rst	= 0x0080,	/* software reset */
	Rrra	= 0x0100,	/* read RRA */
	Lcam	= 0x0200,	/* load CAM */

	Dw32	= 0x0020,	/* data width select */
	Sterm	= 0x0400,	/* synchronous termination */
	Lbr	= 0x4000,	/* latched bus retry */
	Efm	= 0x0010,	/* Empty fill mode */
	W14tf	= 0x0003,	/* 14 Word transmit fifo */

	Prx	= 0x0001,	/* packet received ok */
	Fae	= 0x0004,	/* frame alignment error */
	Crc	= 0x0008,	/* CRC error */
	Lpkt	= 0x0040,	/* last packet in rba */
	Bc	= 0x0080,	/* broadcast packet received */
	Pro	= 0x1000,	/* physical promiscuous mode */
	Brd	= 0x2000,	/* accept broadcast packets */
	Rnt	= 0x4000,	/* accept runt packets */
	Err	= 0x8000,	/* accept packets with errors */

	Ptx	= 0x0001,	/* packet transmitted ok */
	Pintr	= 0x8000,	/* programmable interrupt */

	Rfo	= 0x0001,	/* receive fifo overrun */
	MpTally	= 0x0002,	/* missed packet tally counter rollover */
	FaeTally= 0x0004,	/* frame alignment error tally counter rollover */
	CrcTally= 0x0008,	/* Crc tally counter rollover */
	Rbae	= 0x0010,	/* receive buffer area exceeded */
	Rbe	= 0x0020,	/* receive buffer exhausted */
	Rde	= 0x0040,	/* receive descriptors exhausted */
	Txer	= 0x0100,	/* transmit error */
	Txdn	= 0x0200,	/* transmission done */
	Pktrx	= 0x0400,	/* packet received */
	Pint	= 0x0800,	/* programmed interrupt */
	Lcd	= 0x1000,	/* load CAM done */
	Hbl	= 0x2000,	/* CD heartbeat lost */
	Br	= 0x4000,	/* bus retry occurred */
	AllIntr	= 0x7771,	/* all of the above */
};

/*
 * Receive Resource Descriptor.
 */
typedef struct
{
	uchar	pad0[2];
	ushort	ptr0;		/* buffer pointer in the RRA */
	uchar	pad1[2];
	ushort	ptr1;
	uchar	pad2[2];
	ushort	wc0;		/* buffer word count in the RRA */
	uchar	pad3[2];
	ushort	wc1;
} RXrsc;

/*
 * Receive Packet Descriptor.
 */
typedef struct
{
	uchar	pad0[2];
	ushort	status;		/* receive status */
	uchar	pad1[2];
	ushort	count;		/* packet byte count */
	uchar	pad2[2];
	ushort	ptr0;		/* buffer pointer */
	uchar	pad3[2];
	ushort	ptr1;
	uchar	pad4[2];
	ushort	seqno;		/*  */
	uchar	pad5[2];
	ushort	link;		/* descriptor link and EOL */
	uchar	pad6[2];
	ushort	owner;		/* in use */
} RXpkt;

/*
 * Transmit Packet Descriptor.
 */
typedef struct
{
	uchar	pad0[2];
	ushort	status;		/* transmit status */
	uchar	pad1[2];
	ushort	config;		/*  */
	uchar	pad2[2];
	ushort	size;		/* byte count of entire packet */
	uchar	pad3[2];
	ushort	count;		/* fragment count */
	uchar	pad4[2];
	ushort	ptr0;		/* packet pointer */
	uchar	pad5[2];
	ushort	ptr1;
	uchar	pad6[2];
	ushort	fsize;		/* fragment size */
	uchar	pad7[2];
	ushort	link;		/* descriptor link */
} TXpkt;

enum{
	Eol		= 1,	/* end of list bit in descriptor link */
	Host		= 0,	/* descriptor belongs to host */
	Interface	= -1,	/* descriptor belongs to interface */

	Nether		= 1,
	Ntypes=		8,
};

/*
 * CAM Descriptor
 */
typedef struct {
	uchar	pad0[2];
	ushort	cep;		/* CAM entry pointer */
	uchar	pad1[2];
	ushort	cap0;		/* CAM address port 0 */
	uchar	pad2[2];
	ushort	cap1;		/* CAM address port 1 */
	uchar	pad3[2];
	ushort	cap2;		/* CAM address port 2 */
	uchar	pad4[2];
	ushort	ce;		/* CAM enable */
} Cam;

typedef struct Ether Ether;
struct Ether
{
	uchar	ea[6];
	uchar	ba[6];

	Sonic	*sonic;		/* SONIC registers */

	QLock	tlock;		/* lock for grabbing transmitter queue */
	Rendez	tr;		/* wait here for free xmit buffer */
	int	th;		/* first transmit buffer owned by host */	
	int	ti;		/* first transmit buffer owned by interface */

	int	rh;		/* first receive buffer owned by host */
	int	ri;		/* first receive buffer owned by interface */

	RXrsc	rra[Nrb];	/* receive resource area */
	RXpkt	rda[Nrb];	/* receive descriptor area */
	uchar	rb[Nrb][sizeof(Etherpkt)+4];	/* receive buffer area */
	TXpkt	tda[Ntb];	/* transmit descriptor area */
	uchar	tb[Ntb][sizeof(Etherpkt)];	/* transmit buffer area */
	Cam	cda;		/* CAM descriptor area */

	Netif;
};

Ether *ether[Nether];

#define NEXT(x, l)	(((x)+1)%(l))
#define PREV(x, l)	(((x) == 0) ? (l)-1: (x)-1)
#define LS16(addr)	(PADDR(addr) & 0xFFFF)
#define MS16(addr)	((PADDR(addr)>>16) & 0xFFFF)

static void
reset(Ether *ctlr)
{
	int i;

iprint("reset sonic dcr=#%lux mydcr=#%lux\n", RD(dcr), Sterm|Dw32|Lbr|Efm|W14tf);
	/*
	 * Reset the SONIC, toggle the Rst bit.
	 * Set the data config register for synchronous termination
	 * and 32-bit data-path width.
	 * Clear the descriptor and buffer area.
	 */
	WR(cr, Rst);
	WR(dcr, Sterm|Dw32|Lbr|Efm|W14tf);
	WR(cr, 0);

	/*
	 * Initialise the receive resource area (RRA) and
	 * the receive descriptor area (RDA).
	 *
	 * We use a simple scheme of one packet per descriptor.
	 * We achieve this by setting the EOBC register to be
	 * 2 (16-bit words) less than the buffer size;
	 * thus the size of the receive buffers must be sizeof(Etherpkt)+4.
	 * Set up the receive descriptors as a ring.
	 */
	for(i = 0; i < Nrb; i++){
		ctlr->rra[i].wc0 = (sizeof(ctlr->rb[0])/2) & 0xFFFF;
		ctlr->rra[i].wc1 = ((sizeof(ctlr->rb[0])/2)>>16) & 0xFFFF;

		ctlr->rda[i].link = LS16(&ctlr->rda[NEXT(i, Nrb)]);
		ctlr->rda[i].owner = Interface;

		ctlr->rra[i].ptr0 = ctlr->rda[i].ptr0 = LS16(ctlr->rb[i]);
		ctlr->rra[i].ptr1 = ctlr->rda[i].ptr1 = MS16(ctlr->rb[i]);
	}

	/*
	 * Terminate the receive descriptor ring
	 * and load the SONIC registers to describe the RDA.
	 */
	ctlr->rda[Nrb-1].link |= Eol;

	WR(crda, LS16(ctlr->rda));
	WR(urda, MS16(ctlr->rda));
	WR(eobc, sizeof(ctlr->rb[0])/2 - 2);

	/*
	 * Load the SONIC registers to describe the RRA.
	 * We set the rwp to beyond the area delimited by rsa and
	 * rea. This means that since we've already allocated all
	 * the buffers, we'll never get a 'receive buffer area
	 * exhausted' interrupt and the rrp will just wrap round.
	 * Tell the SONIC to load the RRA and wait for
	 * it to complete.
	 */
	WR(urra, MS16(&ctlr->rra[0]));
	WR(rsa, LS16(&ctlr->rra[0]));
	WR(rrp, LS16(&ctlr->rra[0]));
	WR(rea, LS16(&ctlr->rra[Nrb]));
	WR(rwp, LS16(&ctlr->rra[Nrb+1]));

iprint("wait rra\n");
	WR(cr, Rrra);
	while(RD(cr) & Rrra)
		;
iprint("rra done\n");

	/*
	 * Initialise the transmit descriptor area (TDA).
	 * Each descriptor describes one packet, we make no use
	 * of having the packet in multiple fragments.
	 * The descriptors are linked in a ring; overlapping transmission
	 * with buffer queueing will cause some packets to
	 * go out back-to-back.
	 *
	 * Load the SONIC registers to describe the TDA.
	 */
	for(i = 0; i < Ntb; i++){
		ctlr->tda[i].status = Host;
		ctlr->tda[i].config = 0;
		ctlr->tda[i].count = 1;
		ctlr->tda[i].ptr0 = LS16(ctlr->tb[i]);
		ctlr->tda[i].ptr1 = MS16(ctlr->tb[i]);
		ctlr->tda[i].link = LS16(&ctlr->tda[NEXT(i, Ntb)]);
	}

	WR(ctda, LS16(&ctlr->tda[0]));
	WR(utda, MS16(&ctlr->tda[0]));

	/*
	 * Initialise the software receive and transmit
	 * ring indexes.
	 */
	ctlr->rh = 0;
	ctlr->ri = 0;
	ctlr->th = 0;
	ctlr->ti = 0;

	/*
	 * Initialise the CAM descriptor area (CDA).
	 * We only have one ethernet address to load,
	 * broadcast is defined by the SONIC as all 1s.
	 *
	 * Load the SONIC registers to describe the CDA.
	 * Tell the SONIC to load the CDA and wait for it
	 * to complete.
	 */
	ctlr->cda.cep = 0;
	ctlr->cda.cap0 = (ctlr->ea[1]<<8)|ctlr->ea[0];
	ctlr->cda.cap1 = (ctlr->ea[3]<<8)|ctlr->ea[2];
	ctlr->cda.cap2 = (ctlr->ea[5]<<8)|ctlr->ea[4];
	ctlr->cda.ce = 1;

	WR(cdp, LS16(&ctlr->cda));
	WR(cdc, 1);

	WR(cr, Lcam);
	while(RD(cr) & Lcam)
		;

	/*
	 * Configure the receive control, transmit control
	 * and interrupt-mask registers.
	 * The SONIC is now initialised, but not enabled.
	 */
	WR(rcr, Err|Rnt|Brd);
	WR(tcr, 0);
	WR(imr, AllIntr);
iprint("reset done\n");
}

void
etherintr(void)
{
	int x;
	ushort t;
	Ether *ctlr;
	ulong status;
	TXpkt *txpkt;
	RXpkt *rxpkt;
	Etherpkt *p;
	Netfile *f, **fp;

	ctlr = ether[0];

	for(;;) {
		status = RD(isr) & AllIntr;
		if(status == 0)
			break;

		WR(isr, status);
	
		/*
		 * Transmission complete, for good or bad.
		 */
		if(status & (Txdn|Txer)){
			txpkt = &ctlr->tda[ctlr->ti];
			while(txpkt->status != Host){
				if(txpkt->status == Interface){
					WR(ctda, LS16(txpkt));
					WR(cr, Txp);
					break;
				}
	
				if((txpkt->status & Ptx) == 0)
					ctlr->oerrs++;
	
				txpkt->status = Host;
				ctlr->ti = NEXT(ctlr->ti, Ntb);
				txpkt = &ctlr->tda[ctlr->ti];
			}
			status &= ~(Txdn|Txer);
		}

		if((status & (Pktrx|Rde)) == 0)
			goto noinput;

		/*
		 * A packet arrived or we ran out of descriptors.
		 */
		status &= ~(Pktrx|Rde);
		rxpkt = &ctlr->rda[ctlr->rh];
		while(rxpkt->owner == Host){
			ctlr->inpackets++;
	
			/*
			 * If the packet was received OK, pass it up,
			 * otherwise log the error.
			 * SONIC gives us the CRC in the packet, so
			 * remember to subtract it from the length.
			 */

			if(rxpkt->status & Prx) {
				x = (rxpkt->count & 0xFFFF)-4;
				p = (Etherpkt*)ctlr->rb[ctlr->rh];
				t = (p->type[0]<<8) | p->type[1];
				for(fp = ctlr->f; fp < &ctlr->f[Ntypes]; fp++){
					f = *fp;
					if(f == 0)
						continue;
					if(f->type == t || f->type < 0)
						qproduce(f->in, p->d, x);
				}
			}
			else
			if(rxpkt->status & Fae)
				ctlr->frames++;
			else
			if(rxpkt->status & Crc)
				ctlr->crcs++;
			else
				ctlr->buffs++;
	
			/*
			 * Finished with this packet, it becomes the
			 * last free packet in the ring, so give it Eol,
			 * and take the Eol bit off the previous packet.
			 * Move the ring index on.
			 */
			rxpkt->link |= Eol;
			rxpkt->owner = Interface;
			ctlr->rda[PREV(ctlr->rh, Nrb)].link &= ~Eol;
			ctlr->rh = NEXT(ctlr->rh, Nrb);
	
			rxpkt = &ctlr->rda[ctlr->rh];
		}
		status &= ~(Pktrx|Rde);

	noinput:
		/*
		 * We get a 'load CAM done' interrupt
		 * after initialisation. Ignore it.
		 */
		if(status & Lcd)
			status &= ~Lcd;
	
		/*
		 * Warnings that something is afoot.
		 */
		if(status & Hbl){
			print("sonic: cd heartbeat lost\n");
			status &= ~Hbl;
		}
		if(status & Br){
			print("sonic: bus retry occurred\n");
			status &= ~Br;
		}
	
		if(status & AllIntr)
			print("sonic %ux\n", status);
	}
}

/*
 *  turn promiscuous mode on/off
 */
static void
promiscuous(void *arg, int on)
{
	ushort reg;

	USED(arg);

	reg = RD(rcr);
	if(on)
		WR(rcr, reg|Pro);
	else
		WR(rcr, reg&~Pro);
}

void
etherreset(void)
{
	Ether *ctlr;

	/*
	 * Map the device registers and allocate
	 * memory for the receive/transmit rings.
	 * Set the physical ethernet address and
	 * prime the interrupt handler.
	 */
	if(ether[0] == 0) {
		ether[0] = xspanalloc(sizeof(Ether), BY2PG, 64*1024);
/*		memmove(ether[0]->ea, eeprom.ea, sizeof(ether[0]->ea)); */
	}
	ctlr = ether[0];

	reset(ctlr);

	memset(ctlr->ba, 0xFF, sizeof(ctlr->ba));

	/* general network interface structure */
	netifinit(ether[0], "ether", Ntypes, 32*1024);
	ether[0]->alen = 6;
	memmove(ether[0]->addr, ether[0]->ea, 6);
	memmove(ether[0]->bcast, ctlr->ba, 6);
	ether[0]->promiscuous = promiscuous;
	ether[0]->arg = ether[0];
}

void
etherinit(void)
{
}

Chan*
etherattach(char *spec)
{
	return devattach('l', spec);
}

Chan*
etherclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
etherwalk(Chan *c, char *name)
{
	return netifwalk(ether[0], c, name);
}

Chan*
etheropen(Chan *c, int omode)
{
	return netifopen(ether[0], c, omode);
}

void
ethercreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
}

void
etherclose(Chan *c)
{
	netifclose(ether[0], c);
}

long
etherread(Chan *c, void *buf, long n, ulong offset)
{
	return netifread(ether[0], c, buf, n, offset);
}

static int
isoutbuf(void *arg)
{
	Ether *ctlr = arg;

	return ctlr->tda[ctlr->th].status == Host;
}

long
etherwrite(Chan *c, void *buf, long n, ulong offset)
{
	Etherpkt *p;
	TXpkt *txpkt;
	Ether *ctlr = ether[0];

	USED(offset);

	if(n > ETHERMAXTU)
		error(Ebadarg);

	/* etherif.c handles structure */
	if(NETTYPE(c->qid.path) != Ndataqid)
		return netifwrite(ether[0], c, buf, n);

	/* we handle data */
	qlock(&ctlr->tlock);
	tsleep(&ctlr->tr, isoutbuf, ctlr, 10000);
	if(!isoutbuf(ctlr)){
		print("ether transmitter jammed\n");
	}
	else {
		p =(Etherpkt*)ctlr->tb[ctlr->th];
		memmove(p->d, buf, n);
		if(n < 60) {
			memset(p->d+n, 0, 60-n);
			n = 60;
		}
		memmove(p->s, ctlr->ea, sizeof(ctlr->ea));

		txpkt = &ctlr->tda[ctlr->th];
		txpkt->size = n;
		txpkt->fsize = n;
		txpkt->link |= Eol;
		txpkt->status = Interface;
		ctlr->tda[PREV(ctlr->th, Ntb)].link &= ~Eol;

		ctlr->th = NEXT(ctlr->th, Ntb);
		WR(cr, Txp);
	}
	qunlock(&ctlr->tlock);
	return n;
}

void
etherremove(Chan *c)
{
	USED(c);
}

void
etherstat(Chan *c, char *dp)
{
	netifstat(ether[0], c, dp);
}

void
etherwstat(Chan *c, char *dp)
{
	netifwstat(ether[0], c, dp);
}
