/*
 * Intel 82557 Fast Ethernet PCI Bus LAN Controller
 * as found on the Intel EtherExpress PRO/100B. This chip is full
 * of smarts, unfortunately none of them are in the right place.
 * To do:
 *	the PCI scanning code could be made common to other adapters;
 *	PCI code needs rewritten to handle byte, word, dword accesses
 *	  and using the devno as a bus+dev+function triplet;
 *	tidy/fix locking;
 *	optionally use memory-mapped registers;
 *	stats.
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

	Etherpkt;
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

typedef struct Cb {
	int	command;
	ulong	link;
	uchar	data[24];	/* CbIAS + CbConfigure */
} Cb;

typedef struct TxCB {
	int	command;
	ulong	link;
	ulong	tbd;
	ushort	count;
	uchar	threshold;
	uchar	number;
} TxCB;

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

	CbSF		= 0x00080000,	/* CbTransmit */

	CbI		= 0x20000000,	/* Interrupt after completion */
	CbS		= 0x40000000,	/* Suspend after completion */
	CbEL		= 0x80000000,	/* End of List */
};

enum {					/* CbTransmit count */
	CbEOF		= 0x00008000,
};

typedef struct Ctlr {
	int	port;

	int	ctlrno;
	char*	type;

	uchar	configdata[24];

	Lock	rlock;

	Block*	rfd[Nrfd];
	int	rfdl;
	int	rfdx;

	Lock	cbqlock;
	Block*	cbqhead;
	Block*	cbqtail;
	int	cbqbusy;
} Ctlr;

static uchar configdata[24] = {
	0x16,				/* byte count */
	0x44,				/* Rx/Tx FIFO limit */
	0x00,				/* adaptive IFS */
	0x00,	
	0x04,				/* Rx DMA maximum byte count */
	0x84,				/* Tx DMA maximum byte count */
	0x33,				/* late SCB, CNA interrupts */
	0x01,				/* discard short Rx frames */
	0x00,				/* 503/MII */

	0x00,	
	0x2E,				/* normal operation, NSAI */
	0x00,				/* linear priority */
	0x60,				/* inter-frame spacing */
	0x00,	
	0xF2,	
	0x48,				/* promiscuous mode off */
	0x00,	
	0x40,	
	0xF2,				/* transmit padding enable */
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

static void
custart(Ctlr* ctlr)
{
	if(ctlr->cbqhead == 0){
		ctlr->cbqbusy = 0;
		return;
	}
	ctlr->cbqbusy = 1;

	csr32w(ctlr, General, PADDR(ctlr->cbqhead->rp));
	while(csr8r(ctlr, Command))
		;
	csr8w(ctlr, Command, CUstart);
}

static void
action(Ctlr* ctlr, Block* bp)
{
	Cb *cb;

	ilock(&ctlr->cbqlock);
	cb = (Cb*)bp->rp;
	cb->command |= CbEL;

	if(ctlr->cbqhead){
		ctlr->cbqtail->next = bp;
		cb = (Cb*)ctlr->cbqtail->rp;
		cb->link = PADDR(bp->rp);
		cb->command &= ~CbEL;
	}
	else
		ctlr->cbqhead = bp;
	ctlr->cbqtail = bp;

	if(ctlr->cbqbusy == 0)
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
		csr32w(ctlr, General, PADDR(ctlr->rfd[ctlr->rfdx]->rp));
		while(csr8r(ctlr, Command))
			;
		csr8w(ctlr, Command, RUstart);
	}
	iunlock(&ctlr->rlock);
}

static void
configure(void* arg, int promiscuous)
{
	Ctlr *ctlr;
	Block *bp;
	Cb *cb;

	ctlr = ((Ether*)arg)->ctlr;

	bp = allocb(sizeof(Cb));
	cb = (Cb*)bp->rp;
	bp->wp += sizeof(Cb);

	cb->command = CbConfigure;
	cb->link = NullPointer;
	memmove(cb->data, ctlr->configdata, sizeof(ctlr->configdata));
	if(promiscuous)
		cb->data[15] |= 0x01;
	action(ctlr, bp);
}

static long
write(Ether* ether, void* buf, long n)
{
	Block *bp;
	TxCB *txcb;

	bp = allocb(n+sizeof(TxCB));
	txcb = (TxCB*)bp->wp;
	bp->wp += sizeof(TxCB);

	txcb->command = CbTransmit;
	txcb->link = NullPointer;
	txcb->tbd = NullPointer;
	txcb->count = CbEOF|n;
	txcb->threshold = 2;
	txcb->number = 0;

	memmove(bp->wp, buf, n);
	memmove(bp->wp+Eaddrlen, ether->ea, Eaddrlen);
	bp->wp += n;

	action(ether->ctlr, bp);

	ether->outpackets++;

	return n;
}

static void
interrupt(Ureg*, void* arg)
{
	Rfd *rfd;
	Block *bp;
	Ctlr *ctlr;
	Ether *ether;
	int status;

	ether = arg;
	ctlr = ether->ctlr;

	for(;;){
		status = csr16r(ctlr, Status);
		csr8w(ctlr, Ack, (status>>8) & 0xFF);

		if((status & (StatCX|StatFR|StatCNA|StatRNR)) == 0)
			return;

		if(status & StatFR){
			bp = ctlr->rfd[ctlr->rfdx];
			rfd = (Rfd*)bp->rp;
			while(rfd->field & RfdC){
				etherrloop(ether, rfd, rfd->count & 0x3FFF);
				ether->inpackets++;

				/*
				 * Reinitialise the frame for reception and bump
				 * the receive frame processing index;
				 * bump the sentinel index, mark the new sentinel
				 * and clear the old sentinel suspend bit;
				 * set bp and rfd for the next receive frame to
				 * process.
				 */
				rfd->field = 0;
				rfd->count = 0;
				ctlr->rfdx = NEXT(ctlr->rfdx, Nrfd);

				rfd = (Rfd*)ctlr->rfd[ctlr->rfdl]->rp;
				ctlr->rfdl = NEXT(ctlr->rfdl, Nrfd);
				((Rfd*)ctlr->rfd[ctlr->rfdl]->rp)->field |= RfdS;
				rfd->field &= ~RfdS;

				bp = ctlr->rfd[ctlr->rfdx];
				rfd = (Rfd*)bp->rp;
			}
			status &= ~StatFR;
		}

		if(status & StatRNR){
			while(csr8r(ctlr, Command))
				;
			csr8w(ctlr, Command, RUresume);

			status &= ~StatRNR;
		}

		if(status & StatCNA){
			lock(&ctlr->cbqlock);
			while(bp = ctlr->cbqhead){
				if((((Cb*)bp->rp)->command & CbC) == 0)
					break;
				ctlr->cbqhead = bp->next;
				freeb(bp);
			}
			custart(ctlr);
			unlock(&ctlr->cbqlock);

			status &= ~StatCNA;
		}

		if(status & (StatCX|StatFR|StatCNA|StatRNR|StatMDI|StatSWI))
			panic("%s#%d: status %uX\n", ctlr->type,  ctlr->ctlrno, status);
	}
}

static void
ctlrinit(Ctlr* ctlr)
{
	int i;
	Block *bp;
	Rfd *rfd;
	ulong link;

	link = NullPointer;
	for(i = Nrfd-1; i >= 0; i--){
		if(ctlr->rfd[i] == 0){
			bp = allocb(sizeof(Rfd));
			ctlr->rfd[i] = bp;
		}
		else
			bp = ctlr->rfd[i];
		rfd = (Rfd*)bp->rp;

		rfd->field = 0;
		rfd->link = link;
		link = PADDR(rfd);
		rfd->rbd = NullPointer;
		rfd->count = 0;
		rfd->size = sizeof(Etherpkt);
	}
	((Rfd*)ctlr->rfd[Nrfd-1]->rp)->link = PADDR(ctlr->rfd[0]->rp);

	ctlr->rfdl = 0;
	((Rfd*)ctlr->rfd[0]->rp)->field |= RfdS;
	ctlr->rfdx = 2;

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
		if((csr16r(ctlr, Ecr) & EEdo) == 0)
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

typedef struct Adapter Adapter;
struct Adapter {
	Adapter*	next;
	int		port;
	int		irq;
	int		pcidevno;
};
static Adapter *adapter;

static int
i82557(Ether* ether, int* pcidevno)
{
	PCIcfg pcicfg;
	static int devno = 0;
	int i, irq, port;
	Adapter *ap;

	for(;;){
		pcicfg.vid = 0x8086;
		pcicfg.did = 0x1229;
		if((devno = pcimatch(0, devno, &pcicfg)) == -1)
			break;

		port = 0;
		irq = 0;
		for(i = 0; i < 6; i++){
			if((pcicfg.baseaddr[i] & 0x03) != 0x01)
				continue;
			port = pcicfg.baseaddr[i] & ~0x01;
			irq = pcicfg.irq;
			if(ether->port == 0 || ether->port == port){
				ether->irq = irq;
				*pcidevno = devno-1;
				return port;
			}
		}
		if(port == 0)
			continue;

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->irq = irq;
		ap->pcidevno = devno-1;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static int
reset(Ether* ether)
{
	int i, pcidevno, port, x;
	Adapter *ap, **app;
	uchar ea[Eaddrlen];
	Ctlr *ctlr;
	Block *bp;
	Cb *cb;

	/*
	 * Any adapter matches if no ether->port is supplied, otherwise the
	 * ports must match. First see if an adapter that fits the bill has
	 * already been found. If not, scan for another.
	 */
	port = 0;
	pcidevno = -1;
	for(app = &adapter, ap = *app; ap; app = &ap->next, ap = ap->next){
		if(ether->port == 0 || ether->port == ap->port){
			ether->irq = ap->irq;
			pcidevno = ap->pcidevno;
			*app = ap->next;
			free(ap);
			break;
		}
	}
	if(port == 0 && (port = i82557(ether, &pcidevno)) == 0)
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
	ctlr->ctlrno = ether->ctlrno;
	ctlr->type = ether->type;
	ctlr->port = port;

	csr32w(ctlr, Port, 0);
	delay(1);

	pcicfgr(0, pcidevno, 0, 0x04, &x, 4);
	if((x & 0x05) != 0x05)
		print("PCI command = %uX\n", x);

	csr32w(ctlr, General, 0);
	while(csr8r(ctlr, Command))
		;
	csr8w(ctlr, Command, LoadRUB);
	while(csr8r(ctlr, Command))
		;
	csr8w(ctlr, Command, LoadCUB);

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
	 * at the Organizationally Unique Identifier (OUI) in registers 2 and 3
	 * which should be 0x80017.
	 */
	for(i = 1; i < 32; i++){
		if((x = dp83840r(ctlr, i, 2)) == 0xFFFF)
			continue;
		x <<= 6;
		x |= dp83840r(ctlr, i, 3)>>10;
		if(x != 0x80017)
			continue;

		x = dp83840r(ctlr, i, 0x1B);
		if((x & 0x0200) == 0){
			ctlr->configdata[8] = 1;
			ctlr->configdata[15] |= 0x80;
		}
		break;
	}

	/*
	 * Load the chip configuration
	 */
	configure(ether, 0);

	/*
	 * Check if the adapter's station address is to be overridden.
	 * If not, read it from the EEPROM and set in ether->ea prior to loading
	 * the station address with the Individual Address Setup command.
	 */
	memset(ea, 0, Eaddrlen);
	if(memcmp(ea, ether->ea, Eaddrlen) == 0){
		for(i = 0; i < Eaddrlen/2; i++){
			x = hy93c46r(ctlr, i);
			ether->ea[2*i] = x & 0xFF;
			ether->ea[2*i+1] = (x>>8) & 0xFF;
		}
	}

	bp = allocb(sizeof(Cb));
	cb = (Cb*)bp->rp;
	bp->wp += sizeof(Cb);

	cb->command = CbIAS;
	cb->link = NullPointer;
	memmove(cb->data, ether->ea, Eaddrlen);
	action(ctlr, bp);

	/*
	 * Linkage to the generic ethernet driver.
	 */
	ether->port = port;
	ether->attach = attach;
	ether->write = write;
	ether->interrupt = interrupt;

	ether->promiscuous = configure;
	ether->arg = ether;

	return 0;
}

void
ether82557link(void)
{
	addethercard("i82557",  reset);
}
