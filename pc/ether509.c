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
	IDport		= 0x0100,	/* anywhere between 0x0100 and 0x01F0 */

					/* Commands */
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
	Promiscuous	= 0x08,		/* All addresses (promiscuous mode */

					/* Window Register Offsets */
	Command		= 0x0E,		/* all windows */
	Status		= 0x0E,

	EEPROMdata	= 0x0C,		/* window 0 */
	EEPROMcmd	= 0x0A,
	ResourceConfig	= 0x08,
	ConfigControl	= 0x04,

	TxFreeBytes	= 0x0C,		/* window 1 */
	TxStatus	= 0x0B,
	RxStatus	= 0x08,
	Fifo		= 0x00,

					/* Status/Interrupt Bits */
	Latch		= 0x0001,	/* Interrupt Latch */
	Failure		= 0x0002,	/* Adapter Failure */
	TxComplete	= 0x0004,	/* TX Complete */
	TxAvailable	= 0x0008,	/* TX Available */
	RxComplete	= 0x0010,	/* RX Complete */
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

	FIFOdiag	= 0x04,		/* window 4 */

					/* FIFOdiag bits */
	TxOverrun	= 0x0400,	/* TX Overrrun */
	RxOverrun	= 0x0800,	/* RX Overrun */
	RxStatusOverrun	= 0x1000,	/* RX Status Overrun */
	RxUnderrun	= 0x2000,	/* RX Underrun */
	RxReceiving	= 0x8000,	/* RX Receiving */
};

#define COMMAND(port, cmd, a)	outs(port+Command, ((cmd)<<11)|(a))

static void
attach(Ether *ether)
{
	ulong port;

	port = ether->port;
	/*
	 * Set the receiver packet filter for our own and
	 * and broadcast addresses, set the interrupt masks
	 * for all interrupts, and enable the receiver and transmitter.
	 * The only interrupt we should see under normal conditions
	 * is the receiver interrupt. If the transmit FIFO fills up,
	 * we will also see TxAvailable interrupts.
	 */
	COMMAND(port, SetRxFilter, Broadcast|MyEtherAddr);
	COMMAND(port, SetReadZeroMask, AllIntr|Latch);
	COMMAND(port, SetIntrMask, AllIntr|Latch);
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
	ushort status, type;
	int len;
	ulong port;
	Netfile *f, **fp, **ep;

	port = ether->port;
	while(((status = ins(port+RxStatus)) & RxEmpty) == 0){
		/*
		 * If we had an error, log it and continue
		 * without updating the ring.
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
			 * doubleword. We can pick them out 16-bits
			 * at a time (can try 32-bits at a time
			 * later).
			insl(port+Fifo, ether->rpkt, HOWMANY(len, 4));
			 */
			len = (status & RxByteMask);
			inss(port+Fifo, &ether->rpkt, HOWMANY(len, 2));

			/*
			 * Copy the packet to whoever wants it.
			 */
			type = (ether->rpkt.type[0]<<8)|ether->rpkt.type[1];
			ep = &ether->f[Ntypes];
			for(fp = ether->f; fp < ep; fp++) {
				f = *fp;
				if(f && (f->type == type || f->type < 0))
					qproduce(f->in, &ether->rpkt, len);
			}
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

print("W|");
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
	outss(port+Fifo, (uchar*)buf+2*Eaddrlen, (len-2*Eaddrlen)/2);
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
interrupt(Ether *ether)
{
	ushort status, diag;
	uchar txstatus, x;
	ulong port;

	port = ether->port;
	status = ins(port+Status);

print("I%2.2ux|", status);
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
	 * dealt with other than Latch.
	 * Otherwise, acknowledge the interrupt.
	 */
	if(status & AllIntr)
		panic("ether509 interrupt: #%lux, #%ux\n", status, getdiag(ether));

	COMMAND(port, AckIntr, Latch);
}

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

/*
 * Get configuration parameters.
 */
static int
reset(Ether *ether)
{
	int i, ea;
	ushort x, acr;
	ulong port;

	/*
	 * Do the little configuration dance:
	 *
	 * 2. get to command state, reset, then return to command state
	 */
	idseq();
	outb(IDport, 0xC1);
	delay(2);
	idseq();

	/*
	 * 3. Read the Product ID from the EEPROM.
	 *    This is done by writing the IDPort with 0x83 (0x80
	 *    is the 'read EEPROM command, 0x03 is the offset of
	 *    the Product ID field in the EEPROM).
	 *    The data comes back 1 bit at a time.
	 *    We seem to need a delay here between reading the bits.
	 *
	 * If the ID doesn't match the 3C509 ID code, the adapter
	 * probably isn't there, so barf.
	 */
	outb(IDport, 0x83);
	for(x = 0, i = 0; i < 16; i++){
		delay(5);
		x <<= 1;
		x |= inb(IDport) & 0x01;
	}
	if((x & 0xF0FF) != 0x9050)
		return -1;

	/*
	 * 3. Read the Address Configuration from the EEPROM.
	 *    The Address Configuration field is at offset 0x08 in the EEPROM).
	 * 6. Activate the adapter by writing the Activate command
	 *    (0xFF).
	 */
	outb(IDport, 0x88);
	for(acr = 0, i = 0; i < 16; i++){
		delay(20);
		acr <<= 1;
		acr |= inb(IDport) & 0x01;
	}
	outb(IDport, 0xFF);

	/*
	 * 8. Now we can talk to the adapter's I/O base addresses.
	 *    We get the I/O base address from the acr just read.
	 *
	 *    Enable the adapter. 
	 */
	ether->port = (acr & 0x1F)*0x10 + 0x200;
	port = ether->port;
	outb(port+ConfigControl, 0x01);

	/*
	 * Read the IRQ from the Resource Configuration Register
	 * and the ethernet address from the EEPROM.
	 * The EEPROM command is 8bits, the lower 6 bits being
	 * the address offset.
	 */
	ether->irq = (ins(port+ResourceConfig)>>12) & 0x0F;
	for(ea = 0, i = 0; i < 3; i++, ea += 2){
		while(ins(port+EEPROMcmd) & 0x8000)
			;
		outs(port+EEPROMcmd, (2<<6)|i);
		while(ins(port+EEPROMcmd) & 0x8000)
			;
		x = ins(port+EEPROMdata);
		ether->ea[ea] = (x>>8) & 0xFF;
		ether->ea[ea+1] = x & 0xFF;
	}

	/*
	 * Finished with window 0. Now set the ethernet address
	 * in window 2.
	 * Commands have the format 'CCCCCAAAAAAAAAAA' where C
	 * is a bit in the command and A is a bit in the argument.
	 */
	COMMAND(port, SelectWindow, 2);
	for(i = 0; i < 6; i++)
		outb(port+i, ether->ea[i]);

	/*
	 * Finished with window 2.
	 * Set window 1 for normal operation.
	 */
	COMMAND(port, SelectWindow, 1);

	/*
	 * If we have a 10BASE2 transceiver, start the DC-DC
	 * converter. Wait > 800 microseconds.
	 */
	if(((acr>>14) & 0x03) == 0x03){
		COMMAND(port, StartCoax, 0);
		delay(1);
	}

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
	addethercard("3C509",  reset);
}
