/*
 * Etherlink III and Fast EtherLink adapters.
 * To do:
 *	check robustness in the face of errors;
 *	RxEarly and busmaster;
 *	autoSelect;
 *	PCI latency timer and master enable;
 *	errata list.
 *
 * Product ID:
 *	9150 ISA	3C509[B]
 *	9050 ISA	3C509[B]-TP
 *	9450 ISA	3C509[B]-COMBO
 *	9550 ISA	3C509[B]-TPO
 *
 *	9350 EISA	3C579
 *	9250 EISA	3C579-TP
 *
 *	5920 EISA	3C592-[TP|COMBO|TPO]
 *	5970 EISA	3C597-TX	Fast Etherlink 10BASE-T/100BASE-TX
 *	5971 EISA	3C597-T4	Fast Etherlink 10BASE-T/100BASE-T4
 *	5972 EISA	3C597-MII	Fast Etherlink 10BASE-T/MII
 *
 *	5900 PCI	3C590-[TP|COMBO|TPO]
 *	5950 PCI	3C595-TX	Fast Etherlink Shared 10BASE-T/100BASE-TX
 *	5951 PCI	3C595-T4	Fast Etherlink Shared 10BASE-T/100BASE-T4
 *	5952 PCI	3C595-MII	Fast Etherlink 10BASE-T/MII
 *
 *	9058 PCMCIA	3C589[B]-[TP|COMBO]
 *
 *	627C MCA	3C529
 *	627D MCA	3C529-TP
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
	IDport			= 0x0110,	/* anywhere between 0x0100 and 0x01F0 */
};

enum {						/* all windows */
	Command			= 0x000E,
	IntStatus		= 0x000E,
};

enum {						/* Commands */
	GlobalReset		= 0x0000,
	SelectRegisterWindow	= 0x0001,	
	EnableDcConverter	= 0x0002,
	RxDisable		= 0x0003,
	RxEnable		= 0x0004,
	RxReset			= 0x0005,
	TxDone			= 0x0007,	
	RxDiscard		= 0x0008,
	TxEnable		= 0x0009,
	TxDisable		= 0x000A,
	TxReset			= 0x000B,
	RequestInterrupt	= 0x000C,
	AcknowledgeInterrupt	= 0x000D,
	SetInterruptEnable	= 0x000E,
	SetIndicationEnable	= 0x000F,	/* SetReadZeroMask */
	SetRxFilter		= 0x0010,
	SetRxEarlyThresh	= 0x0011,
	SetTxAvailableThresh	= 0x0012,
	SetTxStartThresh	= 0x0013,
	StartDma		= 0x0014,	/* initiate busmaster operation */
	StatisticsEnable	= 0x0015,
	StatisticsDisable	= 0x0016,
	DisableDcConverter	= 0x0017,
	SetTxReclaimThresh	= 0x0018,	/* PIO-only adapters */
	PowerUp			= 0x001B,	/* not all adapters */
	PowerDownFull		= 0x001C,	/* not all adapters */
	PowerAuto		= 0x001D,	/* not all adapters */
};

enum {						/* (Global|Rx|Tx)Reset command bits */
	tpAuiReset		= 0x0001,	/* 10BaseT and AUI transceivers */
	endecReset		= 0x0002,	/* internal Ethernet encoder/decoder */
	networkReset		= 0x0004,	/* network interface logic */
	fifoReset		= 0x0008,	/* FIFO control logic */
	aismReset		= 0x0010,	/* autoinitialise state-machine logic */
	hostReset		= 0x0020,	/* bus interface logic */
	dmaReset		= 0x0040,	/* bus master logic */
	vcoReset		= 0x0080,	/* on-board 10Mbps VCO */

	resetMask		= 0x00FF,
};

enum {						/* SetRxFilter command bits */
	receiveIndividual	= 0x0001,	/* match station address */
	receiveMulticast	= 0x0002,
	receiveBroadcast	= 0x0004,
	receiveAllFrames	= 0x0008,	/* promiscuous */
};

enum {						/* StartDma command bits */
	Upload			= 0x0000,	/* transfer data from adapter to memory */
	Download		= 0x0001,	/* transfer data from memory to adapter */
};

enum {						/* IntStatus bits */
	interruptLatch		= 0x0001,
	hostError		= 0x0002,	/* Adapter Failure */
	txComplete		= 0x0004,
	txAvailable		= 0x0008,
	rxComplete		= 0x0010,
	rxEarly			= 0x0020,
	intRequested		= 0x0040,
	updateStats		= 0x0080,
	transferInt		= 0x0100,	/* Bus Master Transfer Complete */
	busMasterInProgress	= 0x0800,
	commandInProgress	= 0x1000,

	interruptMask		= 0x01FE,
};

#define COMMAND(port, cmd, a)	outs((port)+Command, ((cmd)<<11)|(a))
#define STATUS(port)		ins((port)+IntStatus)

enum {						/* Window 0 - setup */
	Wsetup			= 0x0000,
						/* registers */
	ManufacturerID		= 0x0000,	/* 3C5[08]*, 3C59[27] */
	ProductID		= 0x0002,	/* 3C5[08]*, 3C59[27] */
	ConfigControl		= 0x0004,	/* 3C5[08]*, 3C59[27] */
	AddressConfig		= 0x0006,	/* 3C5[08]*, 3C59[27] */
	ResourceConfig		= 0x0008,	/* 3C5[08]*, 3C59[27] */
	EepromCommand		= 0x000A,
	EepromData		= 0x000C,
						/* AddressConfig Bits */
	autoSelect9		= 0x0080,
	xcvrMask9		= 0xC000,
						/* ConfigControl bits */
	Ena			= 0x0001,
						/* EepromCommand bits */
	EepromReadRegister	= 0x0080,
	EepromBusy		= 0x8000,
};

#define EEPROMCMD(port, cmd, a)	outs((port)+EepromCommand, (cmd)|(a))
#define EEPROMBUSY(port)	(ins((port)+EepromCommand) & EepromBusy)
#define EEPROMDATA(port)	ins((port)+EepromData)

enum {						/* Window 1 - operating set */
	Wop			= 0x0001,
						/* registers */
	Fifo			= 0x0000,
	RxError			= 0x0004,	/* 3C59[0257] only */
	RxStatus		= 0x0008,
	Timer			= 0x000A,
	TxStatus		= 0x000B,
	TxFree			= 0x000C,
						/* RxError bits */
	rxOverrun		= 0x0001,
	runtFrame		= 0x0002,
	alignmentError		= 0x0004,	/* Framing */
	crcError		= 0x0008,
	oversizedFrame		= 0x0010,
	dribbleBits		= 0x0080,
						/* RxStatus bits */
	rxBytes			= 0x1FFF,	/* 3C59[0257] mask */
	rxBytes9		= 0x07FF,	/* 3C5[078]9 mask */
	rxError9		= 0x3800,	/* 3C5[078]9 error mask */
	rxOverrun9		= 0x0000,
	oversizedFrame9		= 0x0800,
	dribbleBits9		= 0x1000,
	runtFrame9		= 0x1800,
	alignmentError9		= 0x2000,	/* Framing */
	crcError9		= 0x2800,
	rxError			= 0x4000,
	rxIncomplete		= 0x8000,
						/* TxStatus Bits */
	txStatusOverflow	= 0x0004,
	maxCollisions		= 0x0008,
	txUnderrun		= 0x0010,
	txJabber		= 0x0020,
	interruptRequested	= 0x0040,
	txStatusComplete	= 0x0080,
};

enum {						/* Window 2 - station address */
	Wstation		= 0x0002,
};

enum {						/* Window 3 - FIFO management */
	Wfifo			= 0x0003,
						/* registers */
	InternalConfig		= 0x0000,	/* 3C509B, 3C589, 3C59[0257] */
	OtherInt		= 0x0004,	/* 3C59[0257] */
	RomControl		= 0x0006,	/* 3C509B, 3C59[27] */
	MacControl		= 0x0006,	/* 3C59[0257] */
	ResetOptions		= 0x0008,	/* 3C59[0257] */
	RxFree			= 0x000A,
						/* InternalConfig bits */
	xcvr10BaseT		= 0x00000000,
	xcvrAui			= 0x00100000,	/* 10BASE5 */
	xcvr10Base2		= 0x00300000,
	xcvr100BaseTX		= 0x00400000,
	xcvr100BaseFX		= 0x00500000,
	xcvrMii			= 0x00600000,
	xcvrMask		= 0x00700000,
	autoSelect		= 0x01000000,
						/* MacControl bits */
	deferExtendEnable	= 0x0001,
	deferTimerSelect	= 0x001E,	/* mask */
	fullDuplexEnable	= 0x0020,
	allowLargePackets	= 0x0040,
						/* ResetOptions bits */
	baseT4Available		= 0x0001,
	baseTXAvailable		= 0x0002,
	baseFXAvailable		= 0x0004,
	base10TAvailable	= 0x0008,
	coaxAvailable		= 0x0010,
	auiAvailable		= 0x0020,
	miiAvailable		= 0x0040,
};

enum {						/* Window 4 - diagnostic */
	Wdiagnostic		= 0x0004,
						/* registers */
	VcoDiagnostic		= 0x0002,
	FifoDiagnostic		= 0x0004,
	NetworkDiagnostic	= 0x0006,
	PhysicalMgmt		= 0x0008,
	MediaStatus		= 0x000A,
	BadSSD			= 0x000C,
						/* FifoDiagnostic bits */
	txOverrun		= 0x0400,
	rxUnderrun		= 0x2000,
	receiving		= 0x8000,
						/* MediaStatus bits */
	dataRate100		= 0x0002,
	crcStripDisable		= 0x0004,
	enableSqeStats		= 0x0008,
	collisionDetect		= 0x0010,
	carrierSense		= 0x0020,
	jabberGuardEnable	= 0x0040,
	linkBeatEnable		= 0x0080,
	jabberDetect		= 0x0200,
	polarityReversed	= 0x0400,
	linkBeatDetect		= 0x0800,
	txInProg		= 0x1000,
	dcConverterEnabled	= 0x4000,
	auiDisable		= 0x8000,
};

enum {						/* Window 5 - internal state */
	Wstate			= 0x0005,
						/* registers */
	TxStartThresh		= 0x0000,
	TxAvalableThresh	= 0x0002,
	RxEarlyThresh		= 0x0006,
	RxFilter		= 0x0008,
	InterruptEnable		= 0x000A,
	IndicationEnable	= 0x000C,
};

enum {						/* Window 6 - statistics */
	Wstatistics		= 0x0006,
						/* registers */
	CarrierLost		= 0x0000,
	SqeErrors		= 0x0001,
	MultipleColls		= 0x0002,
	SingleCollFrames	= 0x0003,
	LateCollisions		= 0x0004,
	RxOverruns		= 0x0005,
	FramesXmittedOk		= 0x0006,
	FramesRcvdOk		= 0x0007,
	FramesDeferred		= 0x0008,
	UpperFramesOk		= 0x0009,
	BytesRcvdOk		= 0x000A,
	BytesXmittedOk		= 0x000C,
};

enum {						/* Window 7 - bus master operations */
	Wmaster			= 0x0007,
						/* registers */
	MasterAddress		= 0x0000,
	MasterLen		= 0x0006,
	MasterStatus		= 0x000C,
						/* MasterStatus bits */
	masterAbort		= 0x0001,
	targetAbort		= 0x0002,
	targetRetry		= 0x0004,
	targetDisc		= 0x0008,
	masterDownload		= 0x1000,
	masterUpload		= 0x4000,
	masterInProgress	= 0x8000,

	masterMask		= 0xD00F,
};	

typedef struct {
	Lock	wlock;				/* window access */

	int	attached;
	int	busmaster;
	Block*	rbp[2];				/* receive buffers */
	int	rbpix;

	Block*	txqhead;			/* transmit queue */
	Block*	txqtail;
	int	txthreshold;
	int	txbusy;

	long	interrupts;			/* statistics */
	long	timer;
	
	long	carrierlost;
	long	sqeerrors;
	long	multiplecolls;
	long	singlecollframes;
	long	latecollisions;
	long	rxoverruns;
	long	framesxmittedok;
	long	framesrcvdok;
	long	framesdeferred;
	long	bytesrcvdok;
	long	bytesxmittedok;
	long	badssd;

	int	xcvr;				/* transceiver type */
	int	rxstatus9;			/* old-style RxStatus register */
	int	ts;				/* threshold shift */
} Ctlr;

static Block*
allocrbp(void)
{
	Block *bp;
	ulong addr;

	/*
	 * The receive buffers must be on a 32-byte
	 * boundary for EISA busmastering.
	 */
	bp = allocb(ROUNDUP(sizeof(Etherpkt), 4) + 31);
	addr = (ulong)bp->base;
	addr = ROUNDUP(addr, 32);
	bp->rp = (uchar*)addr;

	return bp;
}

static uchar*
startdma(Ether* ether, ulong address)
{
	int port, status, w;
	uchar *wp;

	port = ether->port;

	w = (STATUS(port)>>13) & 0x07;
	COMMAND(port, SelectRegisterWindow, Wmaster);

	wp = KADDR(inl(port+MasterAddress));
	status = ins(port+MasterStatus);
	if(status & (masterInProgress|targetAbort|masterAbort))
		print("elnk3#%d: BM status 0x%uX\n", ether->ctlrno, status);
	outs(port+MasterStatus, masterMask);
	outl(port+MasterAddress, address);
	outs(port+MasterLen, sizeof(Etherpkt));
	COMMAND(port, StartDma, Upload);

	COMMAND(port, SelectRegisterWindow, w);
	return wp;
}

static void
promiscuous(void* arg, int on)
{
	int filter, port;

	port = ((Ether*)arg)->port;
	
	filter = receiveBroadcast|receiveIndividual;
	if(on)
		filter |= receiveAllFrames;
	COMMAND(port, SetRxFilter, filter);
}

static void
attach(Ether* ether)
{
	int port, x;
	Ctlr *ctlr;

	ctlr = ether->ctlr;
	ilock(&ctlr->wlock);
	if(ctlr->attached){
		iunlock(&ctlr->wlock);
		return;
	}

	port = ether->port;

	/*
	 * Set the receiver packet filter for this and broadcast addresses,
	 * set the interrupt masks for all interrupts, enable the receiver
	 * and transmitter.
	 */
	promiscuous(ether, ether->prom);

	x = interruptMask|interruptLatch;
	if(ctlr->busmaster)
		x &= ~(rxEarly|rxComplete);
	COMMAND(port, SetIndicationEnable, x);
	COMMAND(port, SetInterruptEnable, x);

	COMMAND(port, RxEnable, 0);
	COMMAND(port, TxEnable, 0);

	/*
	 * Prime the busmaster channel for receiving directly into a
	 * receive packet buffer if necessary.
	 */
	ctlr->rbpix = 0;
	if(ctlr->busmaster)
		startdma(ether, PADDR(ctlr->rbp[ctlr->rbpix]->rp));

	ctlr->attached = 1;
	iunlock(&ctlr->wlock);
}

static void
statistics(Ether* ether)
{
	int port, u, w;
	Ctlr *ctlr;

	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * 3C59[27] require a read between a PIO write and
	 * reading a statistics register.
	 */
	w = (STATUS(port)>>13) & 0x07;
	COMMAND(port, SelectRegisterWindow, Wstatistics);
	STATUS(port);

	ctlr->carrierlost += inb(port+CarrierLost) & 0xFF;
	ctlr->sqeerrors += inb(port+SqeErrors) & 0xFF;
	ctlr->multiplecolls += inb(port+MultipleColls) & 0xFF;
	ctlr->singlecollframes += inb(port+SingleCollFrames) & 0xFF;
	ctlr->latecollisions += inb(port+LateCollisions) & 0xFF;
	ctlr->rxoverruns += inb(port+RxOverruns) & 0xFF;
	ctlr->framesxmittedok += inb(port+FramesXmittedOk) & 0xFF;
	ctlr->framesrcvdok += inb(port+FramesRcvdOk) & 0xFF;
	u = inb(port+UpperFramesOk) & 0xFF;
	ctlr->framesxmittedok += (u & 0x30)<<4;
	ctlr->framesrcvdok += (u & 0x03)<<8;
	ctlr->framesdeferred += inb(port+FramesDeferred) & 0xFF;
	ctlr->bytesrcvdok += ins(port+BytesRcvdOk) & 0xFFFF;
	ctlr->bytesxmittedok += ins(port+BytesXmittedOk) & 0xFFFF;

	if(ctlr->xcvr == xcvr100BaseTX || ctlr->xcvr == xcvr100BaseFX){
		COMMAND(port, SelectRegisterWindow, Wdiagnostic);
		STATUS(port);
		ctlr->badssd += inb(port+BadSSD);
	}

	COMMAND(port, SelectRegisterWindow, w);
}

static void
transmit(Ether* ether)
{
	int port, len;
	Ctlr *ctlr;
	Block *bp;

	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * Attempt to top-up the transmit FIFO. If there's room simply
	 * stuff in the packet length (unpadded to a dword boundary), the
	 * packet data (padded) and remove the packet from the queue.
	 * If there's no room post an interrupt for when there is.
	 * This routine is called both from the top level and from interrupt
	 * level and expects to be called with ctlr->wlock already locked
	 * and the correct register window (Wop) in place.
	 */
	while(bp = ctlr->txqhead){
		len = ROUNDUP(BLEN(bp), 4);
		if(len+4 <= ins(port+TxFree)){
			outl(port+Fifo, BLEN(bp));
			outsl(port+Fifo, bp->rp, len/4);

			ctlr->txqhead = bp->next;
			freeb(bp);

			ether->outpackets++;
		}
		else if(ctlr->txbusy == 0){
			ctlr->txbusy = 1;
			COMMAND(port, SetTxAvailableThresh, len>>ctlr->ts);
			return;
		}
	}
}

static long
write(Ether* ether, void* buf, long n)
{
	Ctlr *ctlr;
	Block *bp;
	int port, w;

	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * Pack the write request up in a buffer, give it a source address
	 * and place it on the end of the transmit queue. The data written to the
	 * FIFO must be padded to a dword boundary, hence the ROUNDUP allocation.
	 * Call transmit() to stuff it into the TxFIFO if possible. 
	 */
	bp = allocb(ROUNDUP(n, 4));
	memmove(bp->wp, buf, n);
	memmove(bp->wp+Eaddrlen, ether->ea, Eaddrlen);
	bp->wp += n;

	ilock(&ctlr->wlock);
	w = (STATUS(port)>>13) & 0x07;
	COMMAND(port, SelectRegisterWindow, Wop);
	if(ctlr->txqhead == 0)
		ctlr->txqhead = bp;
	else
		ctlr->txqtail->next = bp;
	ctlr->txqtail = bp;
	if(ctlr->txbusy == 0)
		transmit(ether);
	COMMAND(port, SelectRegisterWindow, w);
	iunlock(&ctlr->wlock);

	return n;
}

static void
receive(Ether* ether)
{
	int len, port, rxerror, rxstatus;
	Ctlr *ctlr;
	Block *bp;

	port = ether->port;
	ctlr = ether->ctlr;

	while(((rxstatus = ins(port+RxStatus)) & rxIncomplete) == 0){
		if(ctlr->busmaster && (STATUS(port) & busMasterInProgress))
			break;

		/*
		 * If there was an error, log it and continue.
		 * Unfortunately the 3C5[078]9 has the error info in the status register
		 * and the 3C59[0257] implement a separate RxError register.
		 */
		if(rxstatus & rxError){
			if(ctlr->rxstatus9){
				switch(rxstatus & rxError9){

				case rxOverrun9:
					ether->overflows++;
					break;

				case oversizedFrame9:
				case runtFrame9:
					ether->buffs++;
					break;

				case alignmentError9:
					ether->frames++;
					break;

				case crcError9:
					ether->crcs++;
					break;

				}
			}
			else{
				rxerror = inb(port+RxError);
				if(rxerror & rxOverrun)
					ether->overflows++;
				if(rxerror & (oversizedFrame|runtFrame))
					ether->buffs++;
				if(rxerror & alignmentError)
					ether->frames++;
				if(rxerror & crcError)
					ether->crcs++;
			}

			COMMAND(port, RxDiscard, 0);
			while(STATUS(port) & commandInProgress)
				;

			if(ctlr->busmaster)
				startdma(ether, PADDR(ctlr->rbp[ctlr->rbpix]->rp));
		}
		else{
			ether->inpackets++;
			bp = ctlr->rbp[ctlr->rbpix];

			if(ctlr->busmaster == 0){
				len = (rxstatus & rxBytes9);
				bp->wp = bp->rp + len;
				insl(port+Fifo, bp->rp, HOWMANY(len, 4));
			}

			COMMAND(port, RxDiscard, 0);
			while(STATUS(port) & commandInProgress)
				;

			if(ctlr->busmaster){
				ctlr->rbpix ^= 1;
				bp->wp = startdma(ether, PADDR(ctlr->rbp[ctlr->rbpix]->rp));
			}

			etherrloop(ether, (Etherpkt*)bp->rp, BLEN(bp));
		}
	}
}

static void
interrupt(Ureg*, void* arg)
{
	Ether *ether;
	int port, status, txstatus, w, x;
	Ctlr *ctlr;

	ether = arg;
	port = ether->port;
	ctlr = ether->ctlr;

	lock(&ctlr->wlock);
	w = (STATUS(port)>>13) & 0x07;
	COMMAND(port, SelectRegisterWindow, Wop);

	ctlr->interrupts++;
	ctlr->timer += inb(port+Timer) & 0xFF;
	for(;;){
		/*
		 * Clear the interrupt latch.
		 * It's possible to receive a packet and for another
		 * to become complete before exiting the interrupt
		 * handler so this must be done first to ensure another
		 * interrupt will occur.
		 */
		COMMAND(port, AcknowledgeInterrupt, interruptLatch);
		status = STATUS(port);
		if((status & interruptMask) == 0)
			break;

		if(status & hostError){
			/*
			 * Adapter failure, try to find out why, reset if
			 * necessary. What happens if Tx is active and a reset
			 * occurs, need to retransmit? This probably isn't right.
			 */
			COMMAND(port, SelectRegisterWindow, Wdiagnostic);
			x = ins(port+FifoDiagnostic);
			COMMAND(port, SelectRegisterWindow, Wop);
			print("elnk3#%d: status 0x%uX, diag 0x%uX\n",
			    ether->ctlrno, status, x);

			if(x & txOverrun){
				if(ctlr->busmaster == 0)
					COMMAND(port, TxReset, 0);
				else
					COMMAND(port, TxReset, dmaReset);
				COMMAND(port, TxEnable, 0);
				wakeup(&ether->tr);
			}

			if(x & rxUnderrun){
				/*
				 * This shouldn't happen...
				 * Need to restart any busmastering?
				 */
				COMMAND(port, RxReset, 0);
				while(STATUS(port) & commandInProgress)
					;
				COMMAND(port, RxEnable, 0);
			}

			status &= ~hostError;
		}

		if(status & (transferInt|rxComplete)){
			receive(ether);
			status &= ~(transferInt|rxComplete);
		}

		if(status & txComplete){
			/*
			 * Pop the TxStatus stack, accumulating errors.
			 * Adjust the TX start threshold if there was an underrun.
			 * If there was a Jabber or Underrun error, reset
			 * the transmitter, taking care not to reset the dma logic
			 * as a busmaster receive may be in progress.
			 * For all conditions enable the transmitter.
			 */
			txstatus = 0;
			do{
				if(x = inb(port+TxStatus))
					outb(port+TxStatus, 0);
				txstatus |= x;
			}while(STATUS(port) & txComplete);

			if(txstatus & txUnderrun){
				COMMAND(port, SelectRegisterWindow, Wdiagnostic);
				while(ins(port+MediaStatus) & txInProg)
					;
				COMMAND(port, SelectRegisterWindow, Wop);
				if(ctlr->txthreshold < ETHERMAXTU)
					ctlr->txthreshold += ETHERMINTU;
			}

			if(txstatus & (txJabber|txUnderrun)){
				if(ctlr->busmaster == 0)
					COMMAND(port, TxReset, 0);
				else
					COMMAND(port, TxReset, dmaReset);
				while(STATUS(port) & commandInProgress)
					;
				COMMAND(port, SetTxStartThresh, ctlr->txthreshold>>ctlr->ts);
			}
			COMMAND(port, TxEnable, 0);
			ether->oerrs++;
			status &= ~txComplete;
			status |= txAvailable;
		}

		if(status & txAvailable){
			COMMAND(port, AcknowledgeInterrupt, txAvailable);
			ctlr->txbusy = 0;
			transmit(ether);
			status &= ~txAvailable;
		}

		if(status & updateStats){
			statistics(ether);
			status &= ~updateStats;
		}

		/*
		 * Currently, this shouldn't happen.
		 */
		if(status & rxEarly){
			COMMAND(port, AcknowledgeInterrupt, rxEarly);
			status &= ~rxEarly;
		}

		/*
		 * Panic if there are any interrupts not dealt with.
		 */
		if(status & interruptMask)
			panic("elnk3#%d: interrupt mask 0x%uX\n", ether->ctlrno, status);
	}

	COMMAND(port, SelectRegisterWindow, w);
	unlock(&ctlr->wlock);
}

static long
ifstat(Ether* ether, void* a, long n, ulong offset)
{
	Ctlr *ctlr;
	char buf[512];
	int len;

	if(n == 0)
		return 0;

	ctlr = ether->ctlr;

	ilock(&ctlr->wlock);
	statistics(ether);
	iunlock(&ctlr->wlock);

	len = sprint(buf, "interrupts: %ld\n", ctlr->interrupts);
	len += sprint(buf+len, "timer: %ld\n", ctlr->timer);
	len += sprint(buf+len, "carrierlost: %ld\n", ctlr->carrierlost);
	len += sprint(buf+len, "sqeerrors: %ld\n", ctlr->sqeerrors);
	len += sprint(buf+len, "multiplecolls: %ld\n", ctlr->multiplecolls);
	len += sprint(buf+len, "singlecollframes: %ld\n", ctlr->singlecollframes);
	len += sprint(buf+len, "latecollisions: %ld\n", ctlr->latecollisions);
	len += sprint(buf+len, "rxoverruns: %ld\n", ctlr->rxoverruns);
	len += sprint(buf+len, "framesxmittedok: %ld\n", ctlr->framesxmittedok);
	len += sprint(buf+len, "framesrcvdok: %ld\n", ctlr->framesrcvdok);
	len += sprint(buf+len, "framesdeferred: %ld\n", ctlr->framesdeferred);
	len += sprint(buf+len, "bytesrcvdok: %ld\n", ctlr->bytesrcvdok);
	len += sprint(buf+len, "bytesxmittedok: %ld\n", ctlr->bytesxmittedok);
	sprint(buf+len, "badssd: %ld\n", ctlr->badssd);

	return readstr(offset, a, n, buf);
}

typedef struct Adapter Adapter;
struct Adapter {
	Adapter*	next;
	int		port;
	int		irq;
};
static Adapter *adapter;

/*
 * Write two 0 bytes to identify the IDport and then reset the
 * ID sequence. Then send the ID sequence to the card to get
 * the card into command state.
 */
static void
idseq(void)
{
	int i;
	uchar al;
	static int reset, untag;

	/*
	 * One time only:
	 *	reset any adapters listening
	 */
	if(reset == 0){
		outb(IDport, 0);
		outb(IDport, 0);
		outb(IDport, 0xC0);
		delay(20);
		reset = 1;
	}

	outb(IDport, 0);
	outb(IDport, 0);
	for(al = 0xFF, i = 0; i < 255; i++){
		outb(IDport, al);
		if(al & 0x80){
			al <<= 1;
			al ^= 0xCF;
		}
		else
			al <<= 1;
	}

	/*
	 * One time only:
	 *	write ID sequence to get the attention of all adapters;
	 *	untag all adapters.
	 * If a global reset is done here on all adapters it will confuse
	 * any ISA cards configured for EISA mode.
	 */
	if(untag == 0){
		outb(IDport, 0xD0);
		untag = 1;
	}
}

static ulong
activate(void)
{
	int i;
	ushort x, acr;

	/*
	 * Do the little configuration dance:
	 *
	 * 2. write the ID sequence to get to command state.
	 */
	idseq();

	/*
	 * 3. Read the Manufacturer ID from the EEPROM.
	 *    This is done by writing the IDPort with 0x87 (0x80
	 *    is the 'read EEPROM' command, 0x07 is the offset of
	 *    the Manufacturer ID field in the EEPROM).
	 *    The data comes back 1 bit at a time.
	 *    A delay seems necessary between reading the bits.
	 *
	 * If the ID doesn't match, there are no more adapters.
	 */
	outb(IDport, 0x87);
	delay(20);
	for(x = 0, i = 0; i < 16; i++){
		delay(20);
		x <<= 1;
		x |= inb(IDport) & 0x01;
	}
	if(x != 0x6D50)
		return 0;

	/*
	 * 3. Read the Address Configuration from the EEPROM.
	 *    The Address Configuration field is at offset 0x08 in the EEPROM).
	 */
	outb(IDport, 0x88);
	for(acr = 0, i = 0; i < 16; i++){
		delay(20);
		acr <<= 1;
		acr |= inb(IDport) & 0x01;
	}

	return (acr & 0x1F)*0x10 + 0x200;
}

static ulong
tcm509isa(Ether* ether)
{
	int irq, port;
	Adapter *ap;

	/*
	 * Attempt to activate adapters until one matches the
	 * address criteria. If adapter is set for EISA mode (0x3F0),
	 * tag it and ignore. Otherwise, activate it fully.
	 */
	while(port = activate()){
		/*
		 * 6. Tag the adapter so it won't respond in future.
		 */
		outb(IDport, 0xD1);
		if(port == 0x3F0)
			continue;

		/*
		 * 6. Activate the adapter by writing the Activate command
		 *    (0xFF).
		 */
		outb(IDport, 0xFF);
		delay(20);

		/*
		 * 8. Can now talk to the adapter's I/O base addresses.
		 *    Use the I/O base address from the acr just read.
		 *
		 *    Enable the adapter and clear out any lingering status
		 *    and interrupts.
		 */
		while(STATUS(port) & commandInProgress)
			;
		COMMAND(port, SelectRegisterWindow, Wsetup);
		outs(port+ConfigControl, Ena);

		COMMAND(port, TxReset, 0);
		COMMAND(port, RxReset, 0);
		COMMAND(port, AcknowledgeInterrupt, 0xFF);

		irq = (ins(port+ResourceConfig)>>12) & 0x0F;
		if(ether->port == 0 || ether->port == port){
			ether->irq = irq;
			return port;
		}

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->irq = irq;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static int
tcm5XXeisa(Ether* ether)
{
	static int slot = 1;
	ushort x;
	int irq, port;
	Adapter *ap;

	/*
	 * First time through, check if this is an EISA machine.
	 * If not, nothing to do.
	 */
	if(slot == 1 && strncmp((char*)(KZERO|0xFFFD9), "EISA", 4))
		return 0;

	/*
	 * Continue through the EISA slots looking for a match on both
	 * 3COM as the manufacturer and 3C579-* or 3C59[27]-* as the product.
	 * If an adapter is found, select window 0, enable it and clear
	 * out any lingering status and interrupts.
	 */
	while(slot < MaxEISA){
		port = slot++*0x1000;
		if(ins(port+0xC80+ManufacturerID) != 0x6D50)
			continue;
		x = ins(port+0xC80+ProductID);
		if((x & 0xF0FF) != 0x9050 && (x & 0xFF00) != 0x5900)
			continue;

		COMMAND(port, SelectRegisterWindow, Wsetup);
		outs(port+ConfigControl, Ena);

		COMMAND(port, TxReset, 0);
		COMMAND(port, RxReset, 0);
		COMMAND(port, AcknowledgeInterrupt, 0xFF);

		irq = (ins(port+ResourceConfig)>>12) & 0x0F;
		if(ether->port == 0 || ether->port == port){
			ether->irq = irq;
			return port;
		}

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->irq = irq;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static int
tcm59Xpci(Ether* ether)
{
	PCIcfg pcicfg;
	static int devno = 0;
	int irq, port;
	Adapter *ap;

	for(;;){
		pcicfg.vid = 0x10B7;
		pcicfg.did = 0;
		if((devno = pcimatch(0, devno, &pcicfg)) == -1)
			break;
		port = pcicfg.baseaddr[0] & ~0x01;
		COMMAND(port, GlobalReset, 0);
		while(STATUS(port) & commandInProgress)
			;
		irq = pcicfg.irq;
		if(ether->port == 0 || ether->port == port){
			ether->irq = irq;
			return port;
		}

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->irq = irq;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static int
tcm5XXpcmcia(Ether* ether)
{
	if(cistrcmp(ether->type, "3C589") == 0 || cistrcmp(ether->type, "3C562") == 0)
		return ether->port;

	return 0;
}

static int
autoselect(int port, int rxstatus9)
{
	int media, x;

	/*
	 * Pathetic attempt at automatic media selection.
	 * Really just to get the Fast Etherlink 10BASE-T/100BASE-TX
	 * cards operational.
	 */
	media = auiAvailable|coaxAvailable|base10TAvailable;
	if(rxstatus9 == 0){
		COMMAND(port, SelectRegisterWindow, Wfifo);
		media = ins(port+ResetOptions);
	}

	COMMAND(port, SelectRegisterWindow, Wdiagnostic);
	x = ins(port+MediaStatus) & ~(dcConverterEnabled|linkBeatEnable|jabberGuardEnable);
	outs(port+MediaStatus, x);

	if(media & baseTXAvailable){
		/*
		 * Must have InternalConfig register.
		 */
		COMMAND(port, SelectRegisterWindow, Wfifo);
		x = inl(port+InternalConfig) & ~xcvrMask;
		x |= xcvr100BaseTX;
		outl(port+InternalConfig, x);
		COMMAND(port, TxReset, 0);
		while(STATUS(port) & commandInProgress)
			;
		COMMAND(port, RxReset, 0);
		while(STATUS(port) & commandInProgress)
			;

		COMMAND(port, SelectRegisterWindow, Wdiagnostic);
		x = ins(port+MediaStatus);
		outs(port+MediaStatus, linkBeatEnable|jabberGuardEnable|x);
		delay(1);

		if(ins(port+MediaStatus) & linkBeatDetect)
			return xcvr100BaseTX;
		outs(port+MediaStatus, x);
	}

	if(media & base10TAvailable){
		if(rxstatus9 == 0){
			COMMAND(port, SelectRegisterWindow, Wfifo);
			x = inl(port+InternalConfig) & ~xcvrMask;
			x |= xcvr10BaseT;
			outl(port+InternalConfig, x);
		}
		else{
			COMMAND(port, SelectRegisterWindow, Wsetup);
			x = ins(port+AddressConfig) & ~xcvrMask9;
			x |= (xcvr10BaseT>>20)<<14;
			outs(port+AddressConfig, x);
		}
		COMMAND(port, TxReset, 0);
		while(STATUS(port) & commandInProgress)
			;
		COMMAND(port, RxReset, 0);
		while(STATUS(port) & commandInProgress)
			;

		COMMAND(port, SelectRegisterWindow, Wdiagnostic);
		x = ins(port+MediaStatus);
		outs(port+MediaStatus, linkBeatEnable|jabberGuardEnable|x);
		delay(1);

		if(ins(port+MediaStatus) & linkBeatDetect)
			return xcvr10BaseT;
		outs(port+MediaStatus, x);
	}

	/*
	 * Botch.
	 */
	return autoSelect;
}

int
etherelnk3reset(Ether* ether)
{
	int busmaster, i, port, rxearly, rxstatus9, x, xcvr;
	Adapter *ap, **app;
	uchar ea[Eaddrlen];
	Ctlr *ctlr;

	/*
	 * Any adapter matches if no ether->port is supplied, otherwise the
	 * ports must match. First see if an adapter that fits the bill has
	 * already been found. If not, scan for adapter on PCI, EISA and finally
	 * using the little ISA configuration dance. The EISA and ISA scan
	 * routines leave Wsetup mapped.
	 * If an adapter is found save the IRQ and transceiver type.
	 */
	port = 0;
	rxearly = 2044;
	rxstatus9 = 0;
	xcvr = 0;
	for(app = &adapter, ap = *app; ap; app = &ap->next, ap = ap->next){
		if(ether->port == 0 || ether->port == ap->port){
			port = ap->port;
			ether->irq = ap->irq;
			*app = ap->next;
			free(ap);
			break;
		}
	}
	if(port == 0 && (port = tcm5XXpcmcia(ether))){
		xcvr = ((ins(port+AddressConfig) & xcvrMask9)>>14)<<20;
		rxstatus9 = 1;
	}
	else if(port == 0 && (port = tcm59Xpci(ether))){
		COMMAND(port, SelectRegisterWindow, Wfifo);
		rxearly = 8188;
		xcvr = inl(port+InternalConfig) & (autoSelect|xcvrMask);
	}
	else if(port == 0 && (port = tcm5XXeisa(ether))){
		x = ins(port+ProductID);
		if((x & 0xFF00) == 0x5900){
			COMMAND(port, SelectRegisterWindow, Wfifo);
			rxearly = 8188;
			xcvr = inl(port+InternalConfig) & (autoSelect|xcvrMask);
		}
		else{
			x = ins(port+AddressConfig);
			xcvr = ((x & xcvrMask9)>>14)<<20;
			if(x & autoSelect9)
				xcvr |= autoSelect;
			rxstatus9 = 1;
		}
	}
	else if(port == 0 && (port = tcm509isa(ether))){
		x = ins(port+AddressConfig);
		xcvr = ((x & xcvrMask9)>>14)<<20;
		if(x & autoSelect9)
			xcvr |= autoSelect;
		rxstatus9 = 1;
	}

	if(port == 0)
		return -1;

	/*
	 * Check if the adapter's station address is to be overridden.
	 * If not, read it from the EEPROM and set in ether->ea prior to loading the
	 * station address in Wstation. The EEPROM returns 16-bits at a time.
	 */
	memset(ea, 0, Eaddrlen);
	if(memcmp(ea, ether->ea, Eaddrlen) == 0){
		COMMAND(port, SelectRegisterWindow, Wsetup);
		while(EEPROMBUSY(port))
			;
		for(i = 0; i < Eaddrlen/2; i++){
			EEPROMCMD(port, EepromReadRegister, i);
			while(EEPROMBUSY(port))
				;
			x = EEPROMDATA(port);
			ether->ea[2*i] = x>>8;
			ether->ea[2*i+1] = x;
		}
	}

	COMMAND(port, SelectRegisterWindow, Wstation);
	for(i = 0; i < Eaddrlen; i++)
		outb(port+i, ether->ea[i]);

	/*
	 * Enable the transceiver if necessary and determine whether
	 * busmastering can be used. Due to bugs in the first revision
	 * of the 3C59[05], don't use busmastering at 10Mbps.
	 */
	if(xcvr & autoSelect)
		xcvr = autoselect(port, rxstatus9);
	COMMAND(port, SelectRegisterWindow, Wdiagnostic);
	x = ins(port+MediaStatus) & ~(linkBeatEnable|jabberGuardEnable);
	outs(port+MediaStatus, x);
	if(x & dataRate100)
		busmaster = 1;
	else
		busmaster = 0;
	switch(xcvr){

	case xcvr100BaseTX:
	case xcvr100BaseFX:
		ether->mbps = 100;
		/*FALLTHROUGH*/
	case xcvr10BaseT:
		/*
		 * Enable Link Beat and Jabber to start the
		 * transceiver.
		 */
		x |= linkBeatEnable|jabberGuardEnable;
		outs(port+MediaStatus, x);
		break;

	case xcvr10Base2:
		/*
		 * Start the DC-DC converter.
		 * Wait > 800 microseconds.
		 */
		COMMAND(port, EnableDcConverter, 0);
		delay(1);
		break;
	}

	/*
	 * Wop is the normal operating register set.
	 * The 3C59[0257] adapters allow access to more than one register window
	 * at a time, but there are situations where switching still needs to be
	 * done, so just do it.
	 * Clear out any lingering Tx status.
	 */
	COMMAND(port, SelectRegisterWindow, Wop);
	while(inb(port+TxStatus))
		outb(port+TxStatus, 0);

	/*
	 * Allocate a controller structure, clear out the
	 * adapter statistics, clear the statistics logged into ctlr
	 * and enable statistics collection. Xcvr is needed in order
	 * to collect the BadSSD statistics.
	 */
	ether->ctlr = malloc(sizeof(Ctlr));
	ctlr = ether->ctlr;

	ilock(&ctlr->wlock);
	ctlr->xcvr = xcvr;
	statistics(ether);
	memset(ctlr, 0, sizeof(Ctlr));

	ctlr->busmaster = busmaster;
	ctlr->xcvr = xcvr;
	ctlr->rxstatus9 = rxstatus9;
	if(rxearly >= 2048)
		ctlr->ts = 2;

	COMMAND(port, StatisticsEnable, 0);

	/*
	 * Allocate the receive buffers.
	 */
	ctlr->rbpix = 0;
	ctlr->rbp[0] = allocrbp();
	if(ctlr->busmaster)
		ctlr->rbp[1] = allocrbp();

	/*
	 * Set a base TxStartThresh which will be incremented
	 * if any txUnderrun errors occur and ensure no RxEarly
	 * interrupts happen.
	 */
	ctlr->txthreshold = ETHERMINTU*2;
	COMMAND(port, SetTxStartThresh, ctlr->txthreshold>>ctlr->ts);
	COMMAND(port, SetRxEarlyThresh, rxearly>>ctlr->ts);

	iunlock(&ctlr->wlock);

	/*
	 * Linkage to the generic ethernet driver.
	 */
	ether->port = port;
	ether->attach = attach;
	ether->write = write;
	ether->interrupt = interrupt;
	ether->ifstat = ifstat;

	ether->promiscuous = promiscuous;
	ether->arg = ether;

	return 0;
}

void
etherelnk3link(void)
{
	addethercard("elnk3",  etherelnk3reset);
	addethercard("3C509",  etherelnk3reset);
}
