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
	IDport		= 0x0110,	/* anywhere between 0x0100 and 0x01F0 */
					/* Commands */
	GlobalReset	= 0x00,		/* Global Reset */
	SelectWindow	= 0x01,		/* SelectWindow command */
	StartCoax	= 0x02,		/* Start Coaxial Transceiver */
	RxDisable	= 0x03,		/* RX Disable */
	RxEnable	= 0x04,		/* RX Enable */
	RxReset		= 0x05,		/* RX Reset */
	RxDiscard	= 0x08,		/* RX Discard Top Packet */
	TxEnable	= 0x09,		/* TX Enable */
	TxDisable	= 0x0A,		/* TX Disable */
	TxReset		= 0x0B,		/* TX Reset */
	AckIntr		= 0x0D,		/* Acknowledge Interrupt */
	SetIntrMask	= 0x0E,		/* Set Interrupt Mask */
	SetReadZeroMask	= 0x0F,		/* Set Read Zero Mask */
	SetRxFilter	= 0x10,		/* Set RX Filter */
	SetTxAvailable	= 0x12,		/* Set TX Available Threshold */

					/* RX Filter Command Bits */
	MyEtherAddr	= 0x01,		/* Individual address */
	Multicast	= 0x02,		/* Group (multicast) addresses */
	Broadcast	= 0x04,		/* Broadcast address */
	Promiscuous	= 0x08,		/* All addresses (promiscuous mode) */

					/* Window Register Offsets */
	Command		= 0x0E,		/* all windows */
	Status		= 0x0E,

	ManufacturerID	= 0x00,		/* window 0 */
	ProductID	= 0x02,
	ConfigControl	= 0x04,
	AddressConfig	= 0x06,
	ResourceConfig	= 0x08,
	EEPROMcmd	= 0x0A,
	EEPROMdata	= 0x0C,

					/* AddressConfig Bits */
	XcvrTypeMask	= 0xC000,	/* Transceiver Type Select */
	Xcvr10BaseT	= 0x0000,
	XcvrAUI		= 0x4000,
	XcvrBNC		= 0xC000,

					/* ConfigControl */
	Rst		= 0x04,		/* Reset Adapter */
	Ena		= 0x01,		/* Enable Adapter */

	Fifo		= 0x00,		/* window 1 */
	RxStatus	= 0x08,
	Timer		= 0x0A,
	TxStatus	= 0x0B,
	TxFreeBytes	= 0x0C,

					/* Status/Interrupt Bits */
	Latch		= 0x0001,	/* Interrupt Latch */
	Failure		= 0x0002,	/* Adapter Failure */
	TxComplete	= 0x0004,	/* TX Complete */
	TxAvailable	= 0x0008,	/* TX Available */
	RxComplete	= 0x0010,	/* RX Complete */
	Update		= 0x0080,	/* Update Statistics */
	AllIntr		= 0x00FE,	/* All Interrupt Bits */
	CmdInProgress	= 0x1000,	/* Command In Progress */

					/* TxStatus Bits */
	TxJabber	= 0x20,		/* Jabber Error */
	TxUnderrun	= 0x10,		/* Underrun */
	TxMaxColl	= 0x08,		/* Maximum Collisions */

					/* RxStatus Bits */
	RxByteMask	= 0x07FF,	/* RX Bytes (0-1514) */
	RxErrMask	= 0x3800,	/* Type of Error: */
	RxErrOverrun	= 0x0000,	/*   Overrrun */
	RxErrOversize	= 0x0800,	/*   Oversize Packet (>1514) */
	RxErrDribble	= 0x1000,	/*   Dribble Bit(s) */
	RxErrRunt	= 0x1800,	/*   Runt Packet */
	RxErrFraming	= 0x2000,	/*   Alignment (Framing) */
	RxErrCRC	= 0x2800,	/*   CRC */
	RxError		= 0x4000,	/* Error */
	RxEmpty		= 0x8000,	/* Incomplete or FIFO empty */

	InternalCgf	= 0x00,		/* window 3 */

	FIFOdiag	= 0x04,		/* window 4 */
	MediaStatus	= 0x0A,

					/* FIFOdiag bits */
	TxOverrun	= 0x0400,	/* TX Overrrun */
	RxOverrun	= 0x0800,	/* RX Overrun */
	RxStatusOverrun	= 0x1000,	/* RX Status Overrun */
	RxUnderrun	= 0x2000,	/* RX Underrun */
	RxReceiving	= 0x8000,	/* RX Receiving */

					/* MediaStatus bits */
	JabberEna	= 0x0040,	/* Jabber Enabled (writeable) */
	LinkBeatEna	= 0x0080,	/* Link Beat Enabled (writeable) */
	LinkBeatOk	= 0x0800,	/* Valid link beat detected (ro) */
};

#define COMMAND(port, cmd, a)	outs(port+Command, ((cmd)<<11)|(a))

static void
attach(Ether *ether)
{
	ulong port;
	int x;

	port = ether->port;
	/*
	 * Set the receiver packet filter for our own and
	 * and broadcast addresses, set the interrupt masks
	 * for all interrupts, and enable the receiver and transmitter.
	 * The only interrupt we should see under normal conditions
	 * is the receiver interrupt. If the transmit FIFO fills up,
	 * we will also see TxAvailable interrupts.
	 * Disable Update interrupts for now.
	 */
	x = Broadcast|MyEtherAddr;
	if(ether->prom)
		x |= Promiscuous;
	COMMAND(port, SetRxFilter, x);
	COMMAND(port, SetReadZeroMask, (AllIntr|Latch) & ~Update);
	COMMAND(port, SetIntrMask, (AllIntr|Latch) & ~Update);
	COMMAND(port, RxEnable, 0);
	COMMAND(port, TxEnable, 0);
}

static void
promiscuous(void *arg, int on)
{
	ulong port;

	port = ((Ether*)arg)->port;
	if(on)
		COMMAND(port, SetRxFilter, Promiscuous|Broadcast|MyEtherAddr);
	else
		COMMAND(port, SetRxFilter, Broadcast|MyEtherAddr);
}

static void
receive(Ether *ether)
{
	ushort status;
	int len;
	ulong port;

	port = ether->port;
	while(((status = ins(port+RxStatus)) & RxEmpty) == 0){
		/*
		 * If we had an error, log it and continue.
		 */
		if(status & RxError){
			switch(status & RxErrMask){

			case RxErrOverrun:	/* Overrrun */
				ether->overflows++;
				break;

			case RxErrOversize:	/* Oversize Packet (>1514) */
			case RxErrRunt:		/* Runt Packet */
				ether->buffs++;
				break;
			case RxErrFraming:	/* Alignment (Framing) */
				ether->frames++;
				break;

			case RxErrCRC:		/* CRC */
				ether->crcs++;
				break;
			}
		}
		else {
			/*
			 * We have a packet. Read it into the
			 * buffer. The CRC is already stripped off.
			 * Must read len bytes padded to a
			 * doubleword. We can pick them out 32-bits
			 * at a time.
			 */
			ether->inpackets++;
			len = (status & RxByteMask);
			insl(port+Fifo, &ether->rpkt, HOWMANY(len, 4));

			/*
			 * Copy the packet to whoever wants it.
			 */
			etherrloop(ether, &ether->rpkt, len);
		}

		/*
		 * Discard the packet as we're done with it.
		 * Wait for discard to complete.
		 */
		COMMAND(port, RxDiscard, 0);
		while(ins(port+Status) & CmdInProgress)
			;
	}
}

static int
istxfifo(void *arg)
{
	Ether *ether;
	ushort len;
	ulong port;

	ether = arg;
	port = ether->port;
	/*
	 * If there's no room in the FIFO for this packet,
	 * set up an interrupt for when space becomes available.
	 * Output packet must be a multiple of 4 in length and
	 * we need 4 bytes for the preamble.
	 * Assume here that when we are called (via tsleep) that
	 * we are safe from interrupts.
	 */
	len = ROUNDUP(ether->tlen, 4);
	if(len+4 > ins(port+TxFreeBytes)){
		COMMAND(port, SetTxAvailable, len);
		return 0;
	}
	return 1;
}

static long
write(Ether *ether, void *buf, long n)
{
	ushort len;
	ulong port;

	port = ether->port;

	ether->tlen = n;
	len = ROUNDUP(ether->tlen, 4);
	tsleep(&ether->tr, istxfifo, ether, 10000);
	if(len+4 > ins(port+TxFreeBytes)){
		print("ether509: transmitter jammed\n");
		return 0;
	}

	/*
	 * We know there's room, copy the packet to the FIFO.
	 * To save copying the packet into a local buffer just
	 * so we can set the source address, stuff the packet
	 * into the FIFO in 3 pieces.
	 * Transmission won't start until the entire packet is
	 * in the FIFO, so it's OK to fault here.
	 */
	outs(port+Fifo, ether->tlen);
	outs(port+Fifo, 0);
	outss(port+Fifo, buf, Eaddrlen/2);
	outss(port+Fifo, ether->ea, Eaddrlen/2);
	outsl(port+Fifo, (uchar*)buf+2*Eaddrlen, (len-2*Eaddrlen)/4);
	ether->outpackets++;

	return n;
}

static ushort
getdiag(Ether *ether)
{
	ushort bytes;
	ulong port;

	port = ether->port;
	COMMAND(port, SelectWindow, 4);
	bytes = ins(port+FIFOdiag);
	COMMAND(port, SelectWindow, 1);
	return bytes & 0xFFFF;
}

static void
interrupt(Ureg *ur, void *arg)
{
	ushort status, diag;
	uchar txstatus, x;
	ulong port;
	Ether *ether;

	USED(ur);
	ether = arg;
	port = ether->port;

	for(;;){
		/*
		 * Clear the interrupt latch.
		 * It's possible to receive a packet and for another
		 * to become complete before we exit the interrupt
		 * handler so this must be done first to ensure another
		 * interrupt will occur.
		 */
		COMMAND(port, AckIntr, Latch);
		status = ins(port+Status);
		if((status & AllIntr) == 0)
			break;
	
		if(status & Failure){
			/*
			 * Adapter failure, try to find out why.
			 * Reset if necessary.
			 * What happens if Tx is active and we reset,
			 * need to retransmit?
			 * This probably isn't right.
			 */
			diag = getdiag(ether);
			print("ether509: status #%ux, diag #%ux\n", status, diag);
	
			if(diag & TxOverrun){
				COMMAND(port, TxReset, 0);
				COMMAND(port, TxEnable, 0);
				wakeup(&ether->tr);
			}
	
			if(diag & RxUnderrun){
				COMMAND(port, RxReset, 0);
				attach(ether);
			}
	
			return;
		}
	
		if(status & RxComplete){
			receive(ether);
			status &= ~RxComplete;
		}
	
		if(status & TxComplete){
			/*
			 * Pop the TX Status stack, accumulating errors.
			 * If there was a Jabber or Underrun error, reset
			 * the transmitter. For all conditions enable
			 * the transmitter.
			 */
			txstatus = 0;
			do{
				if(x = inb(port+TxStatus))
					outb(port+TxStatus, 0);
				txstatus |= x;
			}while(ins(port+Status) & TxComplete);
	
			if(txstatus & (TxJabber|TxUnderrun))
				COMMAND(port, TxReset, 0);
			COMMAND(port, TxEnable, 0);
			ether->oerrs++;
		}
	
		if(status & (TxAvailable|TxComplete)){
			/*
			 * Reset the Tx FIFO threshold.
			 */
			if(status & TxAvailable){
				COMMAND(port, AckIntr, TxAvailable);
				wakeup(&ether->tr);
			}
			status &= ~(TxAvailable|TxComplete);
		}
	
		/*
		 * Panic if there are any interrupt bits on we haven't
		 * dealt with. Should deal with UP (Update Statistics)
		 * for happier coexistence with Windows drivers.
		 */
		if(status & AllIntr)
			panic("ether509 interrupt: #%lux, #%ux\n", status, getdiag(ether));
	}
}

typedef struct Adapter Adapter;
struct Adapter {
	Adapter*	next;
	ulong		port;
	PCIcfg*		pcicfg;
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
}

static ulong
activate(int tag)
{
	int i;
	ushort x, acr;
	ulong port;

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
	 *    We seem to need a delay here between reading the bits.
	 *
	 * If the ID doesn't match, there are no more adapters.
	 */
	outb(IDport, 0x87);
	for(x = 0, i = 0; i < 16; i++){
		delay(5);
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
	port = (acr & 0x1F)*0x10 + 0x200;

	if(tag){
		/*
		 * 6. Tag the adapter so it won't respond in future.
		 * 6. Activate the adapter by writing the Activate command
		 *    (0xFF).
		 */
		outb(IDport, 0xD1);
		outb(IDport, 0xFF);

		/*
		 * 8. Now we can talk to the adapter's I/O base addresses.
		 *    We get the I/O base address from the acr just read.
		 *
		 *    Enable the adapter.
		 */
		outs(port+ConfigControl, Ena);
	}

	return port;
}

static ulong
tcm509(Ether *ether)
{
	static int untag;
	ulong port;
	Adapter *ap;

	/*
	 * One time only:
	 *	write ID sequence to get the attention of all adapters;
	 *	untag all adapters.
	 * If we do a global reset here on all adapters we'll confuse any
	 * ISA cards configured for EISA mode.
	 */
	if(untag == 0){
		idseq();
		outb(IDport, 0xD0);
		untag = 1;
	}

	/*
	 * Attempt to activate adapters until one matches our
	 * address criteria. If adapter is set for EISA mode,
	 * tag it and ignore. Otherwise, reset the adapter and
	 * activate it fully.
	 */
	while(port = activate(0)){
		if(port == 0x3F0){
			outb(IDport, 0xD1);
			continue;
		}
		outb(IDport, 0xC0);
		delay(2);
		if(activate(1) != port)
			print("tcm509: activate\n");
		
		if(ether->port == 0 || ether->port == port)
			return port;

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static ulong
tcm579(Ether *ether)
{
	static int slot = 1;
	ushort x;
	ulong port;
	Adapter *ap;

	/*
	 * First time through, check if this is an EISA machine.
	 * If not, nothing to do.
	 */
	if(slot == 1 && strncmp((char*)(KZERO|0xFFFD9), "EISA", 4))
		return 0;

	/*
	 * Continue through the EISA slots looking for a match on both
	 * 3COM as the manufacturer and 3C579 or 3C579-TP as the product.
	 * If we find an adapter, select window 0, enable it and clear
	 * out any lingering status and interrupts.
	 * Trying to do a GlobalReset here to re-init the card (as in the
	 * 509 code) doesn't seem to work.
	 */
	while(slot < MaxEISA){
		port = slot++*0x1000;
		if(ins(port+0xC80+ManufacturerID) != 0x6D50)
			continue;
		x = ins(port+0xC80+ProductID);
		if((x & 0xF0FF) != 0x9050/* || (x != 0x9350 && x != 0x9250)*/)
			continue;

		COMMAND(port, SelectWindow, 0);
		outs(port+ConfigControl, Ena);

		COMMAND(port, TxReset, 0);
		COMMAND(port, RxReset, 0);
		COMMAND(port, AckIntr, 0xFF);

		if(ether->port == 0 || ether->port == port)
			return port;

		ap = malloc(sizeof(Adapter));
		ap->port = port;
		ap->next = adapter;
		adapter = ap;
	}

	return 0;
}

static PCIcfg*
tcm590(Ether *ether)
{
	PCIcfg* pcicfg;
	static int devno = 0;
	int port;
	Adapter *ap;

	pcicfg = malloc(sizeof(PCIcfg));
	for(;;){
		pcicfg->vid = 0x10B7;
		pcicfg->did = 0;
		if((devno = pcimatch(0, devno, pcicfg)) == -1)
			break;

		port = pcicfg->baseaddr[0] & ~0x01;
		COMMAND(port, GlobalReset, 0);
		while(ins(port+Status) & CmdInProgress)
			;

		if(ether->port == 0 || ether->port == port)
			return pcicfg;

		ap = malloc(sizeof(Adapter));
		ap->pcicfg = pcicfg;
		ap->port = port;
		ap->next = adapter;
		adapter = ap;

		pcicfg = malloc(sizeof(PCIcfg));
	}
	free(pcicfg);

	return 0;
}

static ulong
tcm589(ISAConf *isa)
{
	if(strcmp(isa->type, "3C589") != 0)
		return 0;

	/*
	 *  The 3com manual register description says that this a noop for
	 *  PCMCIA but the flow chart at the end shows it.
	 */
	COMMAND(isa->port, SelectWindow, 0);
	outs(isa->port+ConfigControl, Ena);
	return isa->port;
}

/*
 * Get configuration parameters.
 */
int
ether509reset(Ether *ether)
{
	int i, eax;
	uchar ea[Eaddrlen];
	ushort x, acr;
	ulong l, port;
	Adapter *ap, **app;
	PCIcfg *pcicfg;

	/*
	 * Any adapter matches if no ether->port is supplied,
	 * otherwise the ports must match.
	 * See if we've already found an adapter that fits
	 * the bill.
	 * If no match then try for an EISA card, an ISA card
	 * and finally for a PCMCIA card.
	 */
	port = 0;
	pcicfg = 0;
	for(app = &adapter, ap = *app; ap; app = &ap->next, ap = ap->next){
		if(ether->port == 0 || ether->port == ap->port){
			port = ap->port;
			pcicfg = ap->pcicfg;
			*app = ap->next;
			free(ap);
			break;
		}
	}
	if(port == 0)
		port = tcm589(ether);
	if(port == 0 && (pcicfg = tcm590(ether)))
		port = pcicfg->baseaddr[0] & ~0x01;
	if(port == 0)
		port = tcm579(ether);
	if(port == 0)
		port = tcm509(ether);
	if(port == 0)
		return -1;

	/*
	 * Read the IRQ from the Resource Configuration Register,
	 * the ethernet address from the EEPROM, and the address configuration.
	 * The EEPROM command is 8 bits, the lower 6 bits being
	 * the address offset.
	 */
	COMMAND(port, SelectWindow, 0);
	if(strcmp(ether->type, "3C589") != 0 && pcicfg == 0)
		ether->irq = (ins(port+ResourceConfig)>>12) & 0x0F;
	for(eax = 0, i = 0; i < 3; i++, eax += 2){
		while(ins(port+EEPROMcmd) & 0x8000)
			;
		outs(port+EEPROMcmd, (2<<6)|i);
		while(ins(port+EEPROMcmd) & 0x8000)
			;
		x = ins(port+EEPROMdata);
		ea[eax] = (x>>8) & 0xFF;
		ea[eax+1] = x & 0xFF;
	}
	if(pcicfg){
		ether->irq = pcicfg->irq;
		COMMAND(port, SelectWindow, 3);
		acr = XcvrAUI;
		l = inl(port+InternalCgf);
		switch(l & 0x700000){

		case 0x000000:
			acr = Xcvr10BaseT;
			break;

		case 0x300000:
			acr = XcvrBNC;
			break;
		}
		free(pcicfg);
	}
	else
		acr = ins(port+AddressConfig);

	/*
	 * Finished with window 0. Now set the ethernet address
	 * in window 2.
	 * Commands have the format 'CCCCCAAAAAAAAAAA' where C
	 * is a bit in the command and A is a bit in the argument.
	 */
	COMMAND(port, SelectWindow, 2);
	if((ether->ea[0]|ether->ea[1]|ether->ea[2]|ether->ea[3]|ether->ea[4]|ether->ea[5]) == 0){
		for(i = 0; i < sizeof(ether->ea); i++)
			ether->ea[i] = ea[i];
	}
	for(i = 0; i < Eaddrlen; i++)
		outb(port+i, ether->ea[i]);

	/*
	 * Enable the transceiver if necessary.
	 */
	COMMAND(port, SelectWindow, 4);
	x = ins(port+MediaStatus) & ~(LinkBeatEna|JabberEna);
	outs(port+MediaStatus, x);
	switch(acr & XcvrTypeMask){

	case Xcvr10BaseT:
		/*
		 * Enable Link Beat and Jabber to start the
		 * transceiver.
		 */
		outs(port+MediaStatus, x|LinkBeatEna|JabberEna);
		break;

	case XcvrBNC:
		/*
		 * Start the DC-DC converter.
		 * Wait > 800 microseconds.
		 */
		COMMAND(port, StartCoax, 0);
		delay(1);
		break;
	}

	/*
	 * Set window 1 for normal operation.
	 * Clear out any lingering Tx status.
	 */
	COMMAND(port, SelectWindow, 1);
	while(inb(port+TxStatus))
		outb(port+TxStatus, 0);

	ether->port = port;

	/*
	 * Set up the software configuration.
	 */
	ether->attach = attach;
	ether->write = write;
	ether->interrupt = interrupt;

	ether->promiscuous = promiscuous;
	ether->arg = ether;

	return 0;
}

void
ether509link(void)
{
	addethercard("3C509",  ether509reset);
}
