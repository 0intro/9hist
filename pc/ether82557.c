/*
 * Intel 82557 Fast Ethernet PCI Bus LAN Controller
 * as found on the Intel EtherExpress PRO/100B. This chip is full
 * of smarts, unfortunately none of them are in the right place.
 * To do:
 *	the PCI scanning code could be made common to other adapters;
 *	auto-negotiation;
 *	optionally use memory-mapped registers.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"

#include "etherif.h"

enum {
	Nrfd		= 64,		/* receive frame area */

	NullPointer	= 0xFFFFFFFF,	/* 82557 NULL pointer */
};

enum {					/* CSR */
	Status		= 0x00,		/* byte or word (word includes Ack) */
	Ack		= 0x01,		/* byte */
	Command		= 0x02,		/* byte or word (word includes Interrupt) */
	Interrupt	= 0x03,		/* byte */
	General		= 0x04,		/* dword */
	Port		= 0x08,		/* dword */
	Fcr		= 0x0C,		/* Flash control register */
	Ecr		= 0x0E,		/* EEPROM control register */
	Mcr		= 0x10,		/* MDI control register */
};

enum {					/* Status */
	RUidle		= 0x0000,
	RUsuspended	= 0x0004,
	RUnoresources	= 0x0008,
	RUready		= 0x0010,
	RUrbd		= 0x0020,	/* bit */
	RUstatus	= 0x003F,	/* mask */

	CUidle		= 0x0000,
	CUsuspended	= 0x0040,
	CUactive	= 0x0080,
	CUstatus	= 0x00C0,	/* mask */

	StatSWI		= 0x0400,	/* SoftWare generated Interrupt */
	StatMDI		= 0x0800,	/* MDI r/w done */
	StatRNR		= 0x1000,	/* Receive unit Not Ready */
	StatCNA		= 0x2000,	/* Command unit Not Active (Active->Idle) */
	StatFR		= 0x4000,	/* Finished Receiving */
	StatCX		= 0x8000,	/* Command eXecuted */
	StatTNO		= 0x8000,	/* Transmit NOT OK */
};

enum {					/* Command (byte) */
	CUnop		= 0x00,
	CUstart		= 0x10,
	CUresume	= 0x20,
	LoadDCA		= 0x40,		/* Load Dump Counters Address */
	DumpSC		= 0x50,		/* Dump Statistical Counters */
	LoadCUB		= 0x60,		/* Load CU Base */
	ResetSA		= 0x70,		/* Dump and Reset Statistical Counters */

	RUstart		= 0x01,
	RUresume	= 0x02,
	RUabort		= 0x04,
	LoadHDS		= 0x05,		/* Load Header Data Size */
	LoadRUB		= 0x06,		/* Load RU Base */
	RBDresume	= 0x07,		/* Resume frame reception */
};

enum {					/* Interrupt (byte) */
	InterruptM	= 0x01,		/* interrupt Mask */
	InterruptSI	= 0x02,		/* Software generated Interrupt */
};

enum {					/* Ecr */
	EEsk		= 0x01,		/* serial clock */
	EEcs		= 0x02,		/* chip select */
	EEdi		= 0x04,		/* serial data in */
	EEdo		= 0x08,		/* serial data out */

	EEstart		= 0x04,		/* start bit */
	EEread		= 0x02,		/* read opcode */

	EEaddrsz	= 6,		/* bits of address */
};

enum {					/* Mcr */
	MDIread		= 0x08000000,	/* read opcode */
	MDIwrite	= 0x04000000,	/* write opcode */
	MDIready	= 0x10000000,	/* ready bit */
	MDIie		= 0x20000000,	/* interrupt enable */
};

typedef struct Rfd {
	int	field;
	ulong	link;
	ulong	rbd;
	ushort	count;
	ushort	size;

	uchar	data[sizeof(Etherpkt)];
} Rfd;

enum {					/* field */
	RfdCollision	= 0x00000001,
	RfdIA		= 0x00000002,	/* IA match */
	RfdRxerr	= 0x00000010,	/* PHY character error */
	RfdType		= 0x00000020,	/* Type frame */
	RfdRunt		= 0x00000080,
	RfdOverrun	= 0x00000100,
	RfdBuffer	= 0x00000200,
	RfdAlignment	= 0x00000400,
	RfdCRC		= 0x00000800,

	RfdOK		= 0x00002000,	/* frame received OK */
	RfdC		= 0x00008000,	/* reception Complete */
	RfdSF		= 0x00080000,	/* Simplified or Flexible (1) Rfd */
	RfdH		= 0x00100000,	/* Header RFD */

	RfdI		= 0x20000000,	/* Interrupt after completion */
	RfdS		= 0x40000000,	/* Suspend after completion */
	RfdEL		= 0x80000000,	/* End of List */
};

enum {					/* count */
	RfdF		= 0x00004000,
	RfdEOF		= 0x00008000,
};

typedef struct Cb Cb;
typedef struct Cb {
	Cb*	next;
	Block*	bp;

	int	command;
	ulong	link;
	union {
		uchar	data[24];	/* CbIAS + CbConfigure */
		struct {
			ulong	tbd;
			ushort	count;
			uchar	threshold;
			uchar	number;

			ulong	tba;
			ushort	tbasz;
			ushort	pad;
		};
	};
} Cb;

enum {					/* action command */
	CbOK		= 0x00002000,	/* DMA completed OK */
	CbC		= 0x00008000,	/* execution Complete */

	CbNOP		= 0x00000000,
	CbIAS		= 0x00010000,	/* Individual Address Setup */
	CbConfigure	= 0x00020000,
	CbMAS		= 0x00030000,	/* Multicast Address Setup */
	CbTransmit	= 0x00040000,
	CbDump		= 0x00060000,
	CbDiagnose	= 0x00070000,
	CbCommand	= 0x00070000,	/* mask */

	CbSF		= 0x00080000,	/* Flexible-mode CbTransmit */

	CbI		= 0x20000000,	/* Interrupt after completion */
	CbS		= 0x40000000,	/* Suspend after completion */
	CbEL		= 0x80000000,	/* End of List */
};

enum {					/* CbTransmit count */
	CbEOF		= 0x00008000,
};

typedef struct Ctlr {
	int	port;
	uchar	configdata[24];

	Lock	rlock;			/* registers */

	Lock	rfdlock;		/* receive side */
	Block*	rfdhead;
	Block*	rfdtail;
	int	nrfd;

	Lock	cbqlock;		/* transmit side */
	Cb*	cbqhead;
	Cb*	cbqtail;
	int	cbqbusy;

	Lock	cbplock;		/* pool of free Cb's */
	Cb*	cbpool;


	Lock	dlock;			/* dump statistical counters */
	ulong	dump[17];
} Ctlr;

static uchar configdata[24] = {
	0x16,				/* byte count */
	0x08,				/* Rx/Tx FIFO limit */
	0x00,				/* adaptive IFS */
	0x00,	
	0x00,				/* Rx DMA maximum byte count */
	0x80,				/* Tx DMA maximum byte count */
	0x32,				/* !late SCB, CNA interrupts */
	0x03,				/* discard short Rx frames */
	0x00,				/* 503/MII */

	0x00,	
	0x2E,				/* normal operation, NSAI */
	0x00,				/* linear priority */
	0x60,				/* inter-frame spacing */
	0x00,	
	0xF2,	
	0xC8,				/* promiscuous mode off */
	0x00,	
	0x40,	
	0xF3,				/* transmit padding enable */
	0x80,				/* full duplex pin enable */
	0x3F,				/* no Multi IA */
	0x05,				/* no Multi Cast ALL */
};

#define csr8r(c, r)	(inb((c)->port+(r)))
#define csr16r(c, r)	(ins((c)->port+(r)))
#define csr32r(c, r)	(inl((c)->port+(r)))
#define csr8w(c, r, b)	(outb((c)->port+(r), (int)(b)))
#define csr16w(c, r, w)	(outs((c)->port+(r), (ushort)(w)))
#define csr32w(c, r, l)	(outl((c)->port+(r), (ulong)(l)))

static Block*
rfdalloc(ulong link)
{
	Block *bp;
	Rfd *rfd;

	if(bp = iallocb(sizeof(Rfd))){
		rfd = (Rfd*)bp->rp;
		rfd->field = 0;
		rfd->link = link;
		rfd->rbd = NullPointer;
		rfd->count = 0;
		rfd->size = sizeof(Etherpkt);
	}

	return bp;
}

static Cb*
cballoc(Ctlr* ctlr, int command)
{
	Cb *cb;

	ilock(&ctlr->cbplock);
	if(cb = ctlr->cbpool){
		ctlr->cbpool = cb->next;
		iunlock(&ctlr->cbplock);
		cb->next = 0;
		cb->bp = 0;
	}
	else{
		iunlock(&ctlr->cbplock);
		cb = smalloc(sizeof(Cb));
	}

	cb->command = command;
	cb->link = NullPointer;

	return cb;
}

static void
cbfree(Ctlr* ctlr, Cb* cb)
{
	ilock(&ctlr->cbplock);
	cb->next = ctlr->cbpool;
	ctlr->cbpool = cb;
	iunlock(&ctlr->cbplock);
}

static void
custart(Ctlr* ctlr)
{
	if(ctlr->cbqhead == 0){
		ctlr->cbqbusy = 0;
		return;
	}
	ctlr->cbqbusy = 1;

	ilock(&ctlr->rlock);
	while(csr8r(ctlr, Command))
		;
	csr32w(ctlr, General, PADDR(&ctlr->cbqhead->command));
	csr8w(ctlr, Command, CUstart);
	iunlock(&ctlr->rlock);
}

static void
action(Ctlr* ctlr, Cb* cb)
{
	Cb* tail;

	cb->command |= CbEL;

	ilock(&ctlr->cbqlock);
	if(ctlr->cbqhead){
		tail = ctlr->cbqtail;
		tail->next = cb;
		tail->link = PADDR(&cb->command);
		tail->command &= ~CbEL;
	}
	else
		ctlr->cbqhead = cb;
	ctlr->cbqtail = cb;

	if(!ctlr->cbqbusy)
		custart(ctlr);
	iunlock(&ctlr->cbqlock);
}

static void
attach(Ether* ether)
{
	int status;
	Ctlr *ctlr;

	ctlr = ether->ctlr;
	ilock(&ctlr->rlock);
	status = csr16r(ctlr, Status);
	if((status & RUstatus) == RUidle){
		while(csr8r(ctlr, Command))
			;
		csr32w(ctlr, General, PADDR(ctlr->rfdhead->rp));
		csr8w(ctlr, Command, RUstart);
	}
	iunlock(&ctlr->rlock);
}

static void
configure(Ctlr* ctlr, int promiscuous)
{
	Cb *cb;

	cb = cballoc(ctlr, CbConfigure);
	memmove(cb->data, ctlr->configdata, sizeof(ctlr->configdata));
	if(promiscuous)
		cb->data[15] |= 0x01;
	action(ctlr, cb);
}

static void
promiscuous(void* arg, int on)
{
	configure(((Ether*)arg)->ctlr, on);
}

static long
ifstat(Ether* ether, void* a, long n, ulong offset)
{
	Ctlr *ctlr;
	char buf[512];
	int len;

	ctlr = ether->ctlr;
	lock(&ctlr->dlock);
	ctlr->dump[16] = 0;

	ilock(&ctlr->rlock);
	while(csr8r(ctlr, Command))
		;
	csr8w(ctlr, Command, DumpSC);
	iunlock(&ctlr->rlock);

	/*
	 * Wait for completion status, should be 0xA005.
	 */
	while(ctlr->dump[16] == 0)
		;

	ether->oerrs = ctlr->dump[1]+ctlr->dump[2]+ctlr->dump[3];
	ether->crcs = ctlr->dump[10];
	ether->frames = ctlr->dump[11];
	ether->buffs = ctlr->dump[12]+ctlr->dump[15];
	ether->overflows = ctlr->dump[13];

	if(n == 0){
		unlock(&ctlr->dlock);
		return 0;
	}

	len = sprint(buf, "transmit good frames: %ld\n", ctlr->dump[0]);
	len += sprint(buf+len, "transmit maximum collisions errors: %ld\n", ctlr->dump[1]);
	len += sprint(buf+len, "transmit late collisions errors: %ld\n", ctlr->dump[2]);
	len += sprint(buf+len, "transmit underrun errors: %ld\n", ctlr->dump[3]);
	len += sprint(buf+len, "transmit lost carrier sense: %ld\n", ctlr->dump[4]);
	len += sprint(buf+len, "transmit deferred: %ld\n", ctlr->dump[5]);
	len += sprint(buf+len, "transmit single collisions: %ld\n", ctlr->dump[6]);
	len += sprint(buf+len, "transmit multiple collisions: %ld\n", ctlr->dump[7]);
	len += sprint(buf+len, "transmit total collisions: %ld\n", ctlr->dump[8]);
	len += sprint(buf+len, "receive good frames: %ld\n", ctlr->dump[9]);
	len += sprint(buf+len, "receive CRC errors: %ld\n", ctlr->dump[10]);
	len += sprint(buf+len, "receive alignment errors: %ld\n", ctlr->dump[11]);
	len += sprint(buf+len, "receive resource errors: %ld\n", ctlr->dump[12]);
	len += sprint(buf+len, "receive overrun errors: %ld\n", ctlr->dump[13]);
	len += sprint(buf+len, "receive collision detect errors: %ld\n", ctlr->dump[14]);
	sprint(buf+len, "receive short frame errors: %ld\n", ctlr->dump[15]);

	unlock(&ctlr->dlock);

	return readstr(offset, a, n, buf);
}

static void
transmit(Ether* ether)
{
	Ctlr *ctlr;
	Block *bp;
	Cb *cb;

	bp = qget(ether->oq);
	if(bp == nil)
		return;

	ctlr = ether->ctlr;

	cb = cballoc(ctlr, CbSF|CbTransmit);
	cb->bp = bp;
	cb->tbd = PADDR(&cb->tba);
	cb->count = 0;
	cb->threshold = 2;
	cb->number = 1;
	cb->tba = PADDR(bp->rp);
	cb->tbasz = BLEN(bp);

	action(ctlr, cb);
}

static void
interrupt(Ureg*, void* arg)
{
	Rfd *rfd;
	Cb* cb;
	Block *bp, *xbp;
	Ctlr *ctlr;
	Ether *ether;
	int status;

	ether = arg;
	ctlr = ether->ctlr;

	for(;;){
		lock(&ctlr->rlock);
		status = csr16r(ctlr, Status);
		csr8w(ctlr, Ack, (status>>8) & 0xFF);
		unlock(&ctlr->rlock);

		if(!(status & (StatCX|StatFR|StatCNA|StatRNR|StatMDI|StatSWI)))
			break;

		if(status & StatFR){
			bp = ctlr->rfdhead;
			rfd = (Rfd*)bp->rp;
			while(rfd->field & RfdC){
				/*
				 * If it's an OK receive frame and a replacement buffer
				 * can be allocated then
				 *	adjust the received buffer pointers for the
				 *	  actual data received;
				 *	initialise the replacement buffer to point to
				 *	  the next in the ring;
				 *	pass the received buffer on for disposal;
				 *	initialise bp to point to the replacement.
				 * If not, just adjust the necessary fields for reuse.
				 */
				if((rfd->field & RfdOK) && (xbp = rfdalloc(rfd->link))){
					bp->rp += sizeof(Rfd)-sizeof(Etherpkt);
					bp->wp = bp->rp + (rfd->count & 0x3FFF);

					xbp->next = bp->next;
					bp->next = 0;

					etheriq(ether, bp, 1);
					bp = xbp;
				}
				else{
					rfd->field = 0;
					rfd->count = 0;
				}

				/*
				 * The ring tail pointer follows the head with with one
				 * unused buffer in between to defeat hardware prefetch;
				 * once the tail pointer has been bumped on to the next
				 * and the new tail has the Suspend bit set, it can be
				 * removed from the old tail buffer.
				 * As a replacement for the current head buffer may have
				 * been allocated above, ensure that the new tail points
				 * to it (next and link).
				 */
				rfd = (Rfd*)ctlr->rfdtail->rp;
				ctlr->rfdtail = ctlr->rfdtail->next;
				ctlr->rfdtail->next = bp;
				((Rfd*)ctlr->rfdtail->rp)->link = PADDR(bp->rp);
				((Rfd*)ctlr->rfdtail->rp)->field |= RfdS;
				rfd->field &= ~RfdS;

				/*
				 * Finally done with the current (possibly replaced)
				 * head, move on to the next and maintain the sentinel
				 * between tail and head.
				 */
				ctlr->rfdhead = bp->next;
				bp = ctlr->rfdhead;
				rfd = (Rfd*)bp->rp;
			}
			status &= ~StatFR;
		}

		if(status & StatRNR){
			lock(&ctlr->rlock);
			while(csr8r(ctlr, Command))
				;
			csr8w(ctlr, Command, RUresume);
			unlock(&ctlr->rlock);

			status &= ~StatRNR;
		}

		if(status & StatCNA){
			lock(&ctlr->cbqlock);
			while(cb = ctlr->cbqhead){
				if(!(cb->command & CbC))
					break;
				ctlr->cbqhead = cb->next;
				if(cb->bp)
					freeb(cb->bp);
				cbfree(ctlr, cb);
			}
			custart(ctlr);
			unlock(&ctlr->cbqlock);

			status &= ~StatCNA;
		}

		if(status & (StatCX|StatFR|StatCNA|StatRNR|StatMDI|StatSWI))
			panic("#l%d: status %uX\n", ether->ctlrno, status);
	}
}

static void
ctlrinit(Ctlr* ctlr)
{
	int i;
	Block *bp;
	Rfd *rfd;
	ulong link;

	/*
	 * Create the Receive Frame Area (RFA) as a ring of allocated
	 * buffers.
	 * A sentinel buffer is maintained between the last buffer in
	 * the ring (marked with RfdS) and the head buffer to defeat the
	 * hardware prefetch of the next RFD and allow dynamic buffer
	 * allocation.
	 */
	link = NullPointer;
	for(i = 0; i < Nrfd; i++){
		bp = rfdalloc(link);
		if(ctlr->rfdhead == nil)
			ctlr->rfdtail = bp;
		bp->next = ctlr->rfdhead;
		ctlr->rfdhead = bp;
		link = PADDR(bp->rp);
	}
	ctlr->rfdtail->next = ctlr->rfdhead;
	rfd = (Rfd*)ctlr->rfdtail->rp;
	rfd->link = PADDR(ctlr->rfdhead->rp);
	rfd->field |= RfdS;
	ctlr->rfdhead = ctlr->rfdhead->next;

	memmove(ctlr->configdata, configdata, sizeof(configdata));
}

static int
dp83840r(Ctlr* ctlr, int phyadd, int regadd)
{
	int mcr, timo;

	/*
	 * DP83840
	 * 10/100Mb/s Ethernet Physical Layer.
	 */
	csr32w(ctlr, Mcr, MDIread|(phyadd<<21)|(regadd<<16));
	mcr = 0;
	for(timo = 10; timo; timo--){
		mcr = csr32r(ctlr, Mcr);
		if(mcr & MDIready)
			break;
		delay(1);
	}

	if(mcr & MDIready)
		return mcr & 0xFFFF;

	return -1;
}

static int
hy93c46r(Ctlr* ctlr, int r)
{
	int i, op, data;

	/*
	 * Hyundai HY93C46 or equivalent serial EEPROM.
	 * This sequence for reading a 16-bit register 'r'
	 * in the EEPROM is taken straight from Section
	 * 2.3.4.2 of the Intel 82557 User's Guide.
	 */
	csr16w(ctlr, Ecr, EEcs);
	op = EEstart|EEread;
	for(i = 2; i >= 0; i--){
		data = (((op>>i) & 0x01)<<2)|EEcs;
		csr16w(ctlr, Ecr, data);
		csr16w(ctlr, Ecr, data|EEsk);
		delay(1);
		csr16w(ctlr, Ecr, data);
		delay(1);
	}

	for(i = EEaddrsz-1; i >= 0; i--){
		data = (((r>>i) & 0x01)<<2)|EEcs;
		csr16w(ctlr, Ecr, data);
		csr16w(ctlr, Ecr, data|EEsk);
		delay(1);
		csr16w(ctlr, Ecr, data);
		delay(1);
		if(!(csr16r(ctlr, Ecr) & EEdo))
			break;
	}

	data = 0;
	for(i = 15; i >= 0; i--){
		csr16w(ctlr, Ecr, EEcs|EEsk);
		delay(1);
		if(csr16r(ctlr, Ecr) & EEdo)
			data |= (1<<i);
		csr16w(ctlr, Ecr, EEcs);
		delay(1);
	}

	csr16w(ctlr, Ecr, 0);

	return data;
}

typedef struct Adapter {
	int	port;
	int	irq;
	int	tbdf;
} Adapter;
static Block* adapter;

static void
i82557adapter(Block** bpp, int port, int irq, int tbdf)
{
	Block *bp;
	Adapter *ap;

	bp = allocb(sizeof(Adapter));
	ap = (Adapter*)bp->rp;
	ap->port = port;
	ap->irq = irq;
	ap->tbdf = tbdf;

	bp->next = *bpp;
	*bpp = bp;
}

static int
i82557pci(Ether* ether)
{
	static Pcidev *p;
	int irq, port;

	while(p = pcimatch(p, 0x8086, 0x1229)){
		/*
		 * bar[0] is the memory-mapped register address (4KB),
		 * bar[1] is the I/O port register address (32 bytes) and
		 * bar[2] is for the flash ROM (1MB).
		 */
		port = p->bar[1] & ~0x01;
		irq = p->intl;
		if(ether->port == 0 || ether->port == port){
			ether->irq = irq;
			ether->tbdf = p->tbdf;
			return port;
		}

		i82557adapter(&adapter, port, irq, p->tbdf);
	}

	return 0;
}

static int
reset(Ether* ether)
{
	int i, port, x;
	Block *bp, **bpp;
	Adapter *ap;
	uchar ea[Eaddrlen];
	Ctlr *ctlr;
	Cb *cb;

	/*
	 * Any adapter matches if no ether->port is supplied, otherwise the
	 * ports must match. First see if an adapter that fits the bill has
	 * already been found. If not, scan for another.
	 */
	port = 0;
	bpp = &adapter;
	for(bp = *bpp; bp; bp = bp->next){
		ap = (Adapter*)bp->rp;
		if(ether->port == 0 || ether->port == ap->port){
			port = ap->port;
			ether->irq = ap->irq;
			ether->tbdf = ap->tbdf;
			*bpp = bp->next;
			freeb(bp);
			break;
		}
	}
	if(port == 0 && (port = i82557pci(ether)) == 0)
		return -1;

	/*
	 * Allocate a controller structure and start to initialise it.
	 * Perform a software reset after which need to ensure busmastering
	 * is still enabled. The EtherExpress PRO/100B appears to leave
	 * the PCI configuration alone (see the 'To do' list above) so punt
	 * for now.
	 * Load the RUB and CUB registers for linear addressing (0).
	 */
	ether->ctlr = malloc(sizeof(Ctlr));
	ctlr = ether->ctlr;
	ctlr->port = port;

	ilock(&ctlr->rlock);
	csr32w(ctlr, Port, 0);
	delay(1);

	while(csr8r(ctlr, Command))
		;
	csr32w(ctlr, General, 0);
	csr8w(ctlr, Command, LoadRUB);

	while(csr8r(ctlr, Command))
		;
	csr8w(ctlr, Command, LoadCUB);

	while(csr8r(ctlr, Command))
		;
	csr32w(ctlr, General, PADDR(ctlr->dump));
	csr8w(ctlr, Command, LoadDCA);
	iunlock(&ctlr->rlock);

	/*
	 * Initialise the receive frame and configuration areas.
	 */
	ctlrinit(ctlr);

	/*
	 * Possibly need to configure the physical-layer chip here, but the
	 * EtherExpress PRO/100B appears to bring it up with a sensible default
	 * configuration. However, should check for the existence of the PHY
	 * and, if found, check whether to use 82503 (serial) or MII (nibble)
	 * mode. Verify the PHY is a National Semiconductor DP83840 by looking
	 * at the Organizationally Unique Identifier (OUI) in registers 2 and
	 * 3 which should be 0x80017.
	 */
	for(i = 1; i < 32; i++){
		if((x = dp83840r(ctlr, i, 2)) == 0xFFFF)
			continue;
		x <<= 6;
		x |= dp83840r(ctlr, i, 3)>>10;
		if(x != 0x80017)
			continue;

		x = dp83840r(ctlr, i, 0x19);
		if(!(x & 0x0040)){
			ether->mbps = 100;
			ctlr->configdata[8] = 1;
			ctlr->configdata[15] &= ~0x80;
		}
		else{
			x = dp83840r(ctlr, i, 0x1B);
			if(!(x & 0x0200)){
				ctlr->configdata[8] = 1;
				ctlr->configdata[15] &= ~0x80;
			}
		}
		break;
	}

	/*
	 * Load the chip configuration
	 */
	configure(ctlr, 0);

	/*
	 * Check if the adapter's station address is to be overridden.
	 * If not, read it from the EEPROM and set in ether->ea prior to loading
	 * the station address with the Individual Address Setup command.
	 */
	memset(ea, 0, Eaddrlen);
	if(!memcmp(ea, ether->ea, Eaddrlen)){
		for(i = 0; i < Eaddrlen/2; i++){
			x = hy93c46r(ctlr, i);
			ether->ea[2*i] = x;
			ether->ea[2*i+1] = x>>8;
		}
	}

	cb = cballoc(ctlr, CbIAS);
	memmove(cb->data, ether->ea, Eaddrlen);
	action(ctlr, cb);


	/*
	 * Linkage to the generic ethernet driver.
	 */
	ether->port = port;
	ether->attach = attach;
	ether->transmit = transmit;
	ether->interrupt = interrupt;
	ether->ifstat = ifstat;

	ether->promiscuous = promiscuous;
	ether->arg = ether;

	return 0;
}

void
ether82557link(void)
{
	addethercard("i82557",  reset);
}
