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

#define RD(rn)		(delay(0), *(ulong*)((ulong)&SONICADDR->rn^4))
#define WR(rn, v)	(delay(0), *(ulong*)((ulong)&SONICADDR->rn^4) = (v))
#define ISquad(s)	if((ulong)s & 0x7) panic("sonic: Quad alignment");

typedef struct Pbuf Pbuf;
struct Pbuf
{
	uchar	d[6];
	uchar	s[6];
	uchar	type[2];
	uchar	data[1500];
	uchar	crc[4];
};

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
	ulong	crba0;		/* DO NOT WRITE THESE */
	ulong	crba1;
	ulong	rbwc0;
	ulong	rbwc1;
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

	Rxbuf	= sizeof(Pbuf)+4,
	Txbuf	= sizeof(Pbuf),
};

/*
 * Receive Resource Descriptor.
 */
typedef struct
{
	ushort	pad1;
	ushort		ptr1;		/* buffer pointer in the RRA */
	ushort  pad2;
	ushort		ptr0;
	ushort  pad3;
	ushort		wc1;		/* buffer word count in the RRA */
	ushort  pad4;
	ushort		wc0;
} RXrsc;

/*
 * Receive Packet Descriptor.
 */
typedef struct
{
	ushort	pad0;
		ushort	count;		/* packet byte count */
	ushort	pad1;
		ushort	status;		/* receive status */
	ushort	pad2;
		ushort	ptr1;		/* buffer pointer */
	ushort	pad3;
		ushort	ptr0;
	ushort  pad4;
		ushort	link;		/* descriptor link and EOL */
	ushort	pad5;
		ushort	seqno;		/*  */
	ulong	pad6;
	ushort  pad7;
		ushort	owner;		/* in use */
} RXpkt;

/*
 * Transmit Packet Descriptor.
 */
typedef struct
{
	ushort	pad1;
		ushort	config;		/*  */
	ushort	pad0;
		ushort	status;		/* transmit status */
	ushort	pad3;
		ushort	count;		/* fragment count */
	ushort	pad2;
		ushort	size;		/* byte count of entire packet */
	ushort	pad5;
		ushort	ptr1;
	ushort	pad4;
		ushort	ptr0;		/* packet pointer */
	ushort	pad7;
		ushort	link;		/* descriptor link */
	ushort	pad6;
		ushort	fsize;		/* fragment size */
} TXpkt;

enum{
	Eol		= 1,	/* end of list bit in descriptor link */
	Host		= 0,	/* descriptor belongs to host */
	Interface	= -1,	/* descriptor belongs to interface */

	Nether		= 1,
	Ntypes		= 8,
};

/*
 * CAM Descriptor
 */
typedef struct
{
	ushort	pad0;
		ushort	cap0;		/* CAM address port 0 */
	ushort	pad1;
		ushort	cep;		/* CAM entry pointer */
	ushort	pad2;
		ushort	cap2;		/* CAM address port 2 */
	ushort	pad3;
		ushort	cap1;		/* CAM address port 1 */
	ulong	pad4;
	ushort	pad5;
		ushort	ce;		/* CAM enable */
} Cam;

typedef struct Ether Ether;
struct Ether
{
	uchar	ea[6];
	uchar	ba[6];

	QLock	tlock;		/* lock for grabbing transmitter queue */
	Rendez	tr;		/* wait here for free xmit buffer */
	int	th;		/* first transmit buffer owned by host */	
	int	ti;		/* first transmit buffer owned by interface */

	int	rh;		/* first receive buffer owned by host */
	int	ri;		/* first receive buffer owned by interface */

	RXrsc	*rra;		/* receive resource area */
	RXpkt	*rda;		/* receive descriptor area */
	TXpkt	*tda;		/* transmit descriptor area */
	Cam	*cda;		/* CAM descriptor area */

	uchar	*rb[Nrb];	/* receive buffer area */
	uchar	*tb[Ntb];	/* transmit buffer area */

	Netif;
};

Ether *ether[Nether];

#define NEXT(x, l)	(((x)+1)%(l))
#define PREV(x, l)	(((x) == 0) ? (l)-1: (x)-1)
#define LS16(addr)	(PADDR(addr) & 0xFFFF)
#define MS16(addr)	((PADDR(addr)>>16) & 0xFFFF)

void sonicswap(void*, int);

static void
wus(ushort *a, ushort v)
{
	a[0] = v;
	a[-1] = v;
}

static void
reset(Ether *ctlr)
{
	int i;
	ushort lolen, hilen, loadr, hiadr;

	/*
	 * Reset the SONIC, toggle the Rst bit.
	 * Set the data config register for synchronous termination
	 * and 32-bit data-path width.
	 * Setup the descriptor and buffer area.
	 */
	WR(cr, Rst);
	WR(dcr, 0x2423);	/* 5-19 Carrera manual */
	WR(cr, 0);

	/*
	 * Initialise the receive resource area (RRA) and
	 * the receive descriptor area (RDA).
	 *
	 * We use a simple scheme of one packet per descriptor.
	 * We achieve this by setting the EOBC register to be
	 * 2 (16-bit words) less than the buffer size;
	 * thus the size of the receive buffers must be sizeof(Pbuf)+4.
	 * Set up the receive descriptors as a ring.
	 */

	lolen = (Rxbuf/2) & 0xFFFF;
	hilen = ((Rxbuf/2)>>16) & 0xFFFF;

	for(i = 0; i < Nrb; i++) {
		wus(&ctlr->rra[i].wc0, lolen);
		wus(&ctlr->rra[i].wc1, hilen);

		ctlr->rda[i].link =  LS16(&ctlr->rda[NEXT(i, Nrb)]);
		ctlr->rda[i].owner = Interface;

		loadr = LS16(ctlr->rb[i]);
		wus(&ctlr->rra[i].ptr0, loadr);
		wus(&ctlr->rda[i].ptr0, loadr);

		hiadr = MS16(ctlr->rb[i]);
		wus(&ctlr->rra[i].ptr1, hiadr);
		wus(&ctlr->rda[i].ptr1, hiadr);
	}

	/*
	 * Check the important resources are QUAD aligned
	 */
	ISquad(ctlr->rra);
	ISquad(ctlr->rda);

	/*
	 * Terminate the receive descriptor ring
	 * and load the SONIC registers to describe the RDA.
	 */
	ctlr->rda[Nrb-1].link |= Eol;

	WR(crda, LS16(ctlr->rda));
	WR(urda, MS16(ctlr->rda));
	WR(eobc, Rxbuf/2 - 2);

	/*
	 * Load the SONIC registers to describe the RRA.
	 * We set the rwp to beyond the area delimited by rsa and
	 * rea. This means that since we've already allocated all
	 * the buffers, we'll never get a 'receive buffer area
	 * exhausted' interrupt and the rrp will just wrap round.
	 */
	WR(urra, MS16(&ctlr->rra[0]));
	WR(rsa, LS16(&ctlr->rra[0]));
	WR(rrp, LS16(&ctlr->rra[0]));
	WR(rea, LS16(&ctlr->rra[Nrb]));
	WR(rwp, LS16(&ctlr->rra[Nrb+1]));

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
	 */
	ctlr->cda->cep = 0;
	ctlr->cda->cap0 = (ctlr->ea[1]<<8)|ctlr->ea[0];
	ctlr->cda->cap1 = (ctlr->ea[3]<<8)|ctlr->ea[2];
	ctlr->cda->cap2 = (ctlr->ea[5]<<8)|ctlr->ea[4];
	ctlr->cda->ce = 1;

	WR(cdp, LS16(ctlr->cda));
	WR(cdc, 1);

	/*
	 * Load the Resource Descriptors and Cam contents
	 */
	WR(cr, Rrra);
	while(RD(cr) & Rrra)
		;

	WR(cr, Lcam);
	while(RD(cr) & Lcam)
		;

	/*
	 * Configure the receive control, transmit control
	 * and interrupt-mask registers.
	 * The SONIC is now initialised, but not enabled.
	 */
	WR(rcr, Brd);
	WR(tcr, 0);
	WR(imr, AllIntr);
}

void
sonicpkt(Ether *ctlr, RXpkt *r, Pbuf *p)
{
	int len;
	ushort type;
	Netfile *f, **fp, **ep;

	/*
	 * Sonic delivers CRC as part of the packet count
	 */
	len = (r->count & 0xFFFF)-4;

	sonicswap(p, len);

	type = (p->type[0]<<8) | p->type[1];
	ep = &ctlr->f[Ntypes];
	for(fp = ctlr->f; fp < ep; fp++) {
		f = *fp;
		if(f && (f->type == type || f->type < 0))
			qproduce(f->in, p->d, len);
	}
}

static int
isoutbuf(void *arg)
{
	Ether *ctlr = arg;

	return ctlr->tda[ctlr->th].status == Host;
}

void
etherintr(void)
{
	Ether *c;
	ushort *s;
	ulong status;
	TXpkt *txpkt;
	RXpkt *rxpkt;

	c = ether[0];

	for(;;) {
		status = RD(isr) & AllIntr;
		if(status == 0)
			break;

		/*
		 * Warnings that something is atoe.
		 */
		if(status & Hbl){
			WR(isr, Hbl);
			status &= ~Hbl;
			print("sonic: cd heartbeat lost\n");
		}
		if(status & Br){
WR(cr, Rst);
			print("sonic: bus retry occurred\n");
(*(void(*)(void))0xA001C020)();
			status &= ~Br;
		}
	
		/*
		 * Transmission complete, for good or bad.
		 */
		if(status & (Txdn|Txer)) {
			txpkt = &c->tda[c->ti];
			while(txpkt->status != Host){
				if(txpkt->status == Interface){
					WR(ctda, LS16(txpkt));
					WR(cr, Txp);
					break;
				}
	
				if((txpkt->status & Ptx) == 0)
					c->oerrs++;
	
				txpkt->status = Host;
				c->ti = NEXT(c->ti, Ntb);
				txpkt = &c->tda[c->ti];
			}
			WR(isr, status & (Txdn|Txer));
			status &= ~(Txdn|Txer);
			if(isoutbuf(c))
				wakeup(&c->tr);
		}

		if((status & (Pktrx|Rde)) == 0)
			goto noinput;

		/*
		 * A packet arrived or we ran out of descriptors.
		 */
		rxpkt = &c->rda[c->rh];
		while(rxpkt->owner == Host){
			c->inpackets++;
	
			/*
			 * If the packet was received OK, pass it up,
			 * otherwise log the error.
			 */
			if(rxpkt->status & Prx)
				sonicpkt(c, rxpkt, (Pbuf*)c->rb[c->rh]);
			else
			if(rxpkt->status & Fae)
				c->frames++;
			else
			if(rxpkt->status & Crc)
				c->crcs++;
			else
				c->buffs++;
	
			rxpkt->status  = 0;
			/*
			 * Finished with this packet, it becomes the
			 * last free packet in the ring, so give it Eol,
			 * and take the Eol bit off the previous packet.
			 * Move the ring index on.
			 */
			wus(&rxpkt->link,  rxpkt->link|Eol);
			rxpkt->owner = Interface;
			s = &c->rda[PREV(c->rh, Nrb)].link;
			wus(s, *s & ~Eol);
			c->rh = NEXT(c->rh, Nrb);
	
			rxpkt = &c->rda[c->rh];
		}
		WR(isr, status & (Pktrx|Rde));
		status &= ~(Pktrx|Rde);

	noinput:
		/*
		 * We get a 'load CAM done' interrupt
		 * after initialisation. Ignore it.
		 */
		if(status & Lcd) {
			WR(isr, Lcd);
			status &= ~Lcd;
		}
	
		if(status & AllIntr) {
			WR(isr, status);
			print("sonic #%lux\n", status);
		}
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

static void
initbufs(Ether *c)
{
	int i;
	uchar *mem, *base;

	/* Put the ethernet buffers in the same place
	 * as the bootrom
	 */
	mem = (void*)(KZERO|0x2000);
	base = mem;
	mem = CACHELINE(uchar, mem);

	/*
	 * Descriptors must be built in uncached space
	 */
	c->rra = UNCACHED(RXrsc, mem);
	mem = QUAD(uchar, mem+Nrb*sizeof(RXrsc));

	c->rda = UNCACHED(RXpkt, mem);
	mem = QUAD(uchar, mem+Nrb*sizeof(RXpkt));

	c->tda = UNCACHED(TXpkt, mem);
	mem = QUAD(uchar, mem+Ntb*sizeof(TXpkt));

	c->cda = UNCACHED(Cam, mem);

	mem = CACHELINE(uchar, mem+sizeof(Cam));
	for(i = 0; i < Nrb; i++) {
		c->rb[i] = UNCACHED(uchar, mem);
		mem += sizeof(Pbuf)+4;
		mem = QUAD(uchar, mem);
	}
	for(i = 0; i < Ntb; i++) {
		c->tb[i] = UNCACHED(uchar, mem);
		mem += sizeof(Pbuf);
		mem = QUAD(uchar, mem);
	}
	if(mem >= base+64*1024)
		panic("sonic init");
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
		ctlr = malloc(sizeof(Ether));
		ether[0] = ctlr;
		initbufs(ctlr);
		enetaddr(ether[0]->ea);
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
	static int enable;

	if(enable == 0) {
		enable = 1;
		WR(cr, Rxen);
	}
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

Block*
etherbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static int
etherloop(Etherpkt *p, long n)
{
	int s, different;
	ushort t;
	Netfile *f, **fp;
	Ether *ctlr = ether[0];

	different = memcmp(p->d, ctlr->ea, sizeof(ctlr->ea));
	if(different && memcmp(p->d, ctlr->bcast, sizeof(p->d)))
		return 0;

	s = splhi();
	t = (p->type[0]<<8) | p->type[1];
	for(fp = ctlr->f; fp < &ctlr->f[Ntypes]; fp++) {
		f = *fp;
		if(f == 0)
			continue;
		if(f->type == t || f->type < 0)
			switch(qproduce(f->in, p->d, n)){
			case -1:
				print("etherloop overflow\n");
				break;
			case -2:
				print("etherloop memory\n");
				break;
			}
	}
	splx(s);
	return !different;
}

long
etherwrite(Chan *c, void *buf, long n, ulong offset)
{
	Pbuf *p;
	ushort *s;
	TXpkt *txpkt;
	Ether *ctlr = ether[0];

	USED(offset);

	/* etherif.c handles structure */
	if(NETTYPE(c->qid.path) != Ndataqid)
		return netifwrite(ether[0], c, buf, n);

	if(n > ETHERMAXTU)
		error(Ebadarg);

	p = buf;
	memmove(p->s, ctlr->ea, sizeof(ctlr->ea));

	/* we handle data */
	if(etherloop(buf, n))
		return n;

	qlock(&ctlr->tlock);
	ctlr->outpackets++;
	if(waserror()) {
		qunlock(&ctlr->tlock);
		nexterror();
	}

	tsleep(&ctlr->tr, isoutbuf, ctlr, 10000);

	if(!isoutbuf(ctlr))
		print("ether transmitter jammed cr #%lux\n", RD(cr));
	else {
		p = (Pbuf*)ctlr->tb[ctlr->th];
		memmove(p->d, buf, n);
		if(n < 60) {
			memset(p->d+n, 0, 60-n);
			n = 60;
		}
		sonicswap(p, n);

		txpkt = &ctlr->tda[ctlr->th];
		txpkt->size = n;
		txpkt->fsize = n;
		wus(&txpkt->link, txpkt->link|Eol);
		txpkt->status = Interface;
		s = &ctlr->tda[PREV(ctlr->th, Ntb)].link;
		wus(s, *s & ~Eol);

		ctlr->th = NEXT(ctlr->th, Ntb);
		WR(cr, Txp);
	}
	poperror();
	qunlock(&ctlr->tlock);

	return n;
}

long
etherbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
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

#define swiz(s)	(s<<24)|((s>>8)&0xff00)|((s<<8)&0xff0000)|(s>>24)

void
sonicswap(void *a, int n)
{
	ulong *p, t0, t1;

	n = ((n+8)/8)*8;
	p = a;
	while(n) {
		t0 = p[0];
		t1 = p[1];
		p[0] = swiz(t1);
		p[1] = swiz(t0);
		p += 2;
		n -= 8;
	}
}
