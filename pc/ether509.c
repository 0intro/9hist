#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

#include "ether.h"

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

#define COMMAND(ctlr, cmd, a)	outs(ctlr->card.io+Command, ((cmd)<<11)|(a))

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
reset(Ctlr *ctlr)
{
	int i, ea;
	ushort x, acr;

	/*
	 * Do the little configuration dance. We only look
	 * at the first card that responds, if we ever have more
	 * than one we'll need to modify this sequence.
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
	ctlr->card.io = (acr & 0x1F)*0x10 + 0x200;
	outb(ctlr->card.io+ConfigControl, 0x01);

	/*
	 * Read the IRQ from the Resource Configuration Register
	 * and the ethernet address from the EEPROM.
	 * The EEPROM command is 8bits, the lower 6 bits being
	 * the address offset.
	 */
	ctlr->card.irq = (ins(ctlr->card.io+ResourceConfig)>>12) & 0x0F;
	for(ea = 0, i = 0; i < 3; i++, ea += 2){
		while(ins(ctlr->card.io+EEPROMcmd) & 0x8000)
			;
		outs(ctlr->card.io+EEPROMcmd, (2<<6)|i);
		while(ins(ctlr->card.io+EEPROMcmd) & 0x8000)
			;
		x = ins(ctlr->card.io+EEPROMdata);
		ctlr->ea[ea] = (x>>8) & 0xFF;
		ctlr->ea[ea+1] = x & 0xFF;
	}

	/*
	 * Finished with window 0. Now set the ethernet address
	 * in window 2.
	 * Commands have the format 'CCCCCAAAAAAAAAAA' where C
	 * is a bit in the command and A is a bit in the argument.
	 */
	COMMAND(ctlr, SelectWindow, 2);
	for(i = 0; i < 6; i++)
		outb(ctlr->card.io+i, ctlr->ea[i]);

	/*
	 * Finished with window 2.
	 * Set window 1 for normal operation.
	 */
	COMMAND(ctlr, SelectWindow, 1);

	/*
	 * If we have a 10BASE2 transceiver, start the DC-DC
	 * converter. Wait > 800 microseconds.
	 */
	if(((acr>>14) & 0x03) == 0x03){
		COMMAND(ctlr, StartCoax, 0);
		delay(1);
	}

	return 0;
}

static void
attach(Ctlr *ctlr)
{
	/*
	 * Set the receiver packet filter for our own and
	 * and broadcast addresses, set the interrupt masks
	 * for all interrupts, and enable the receiver and transmitter.
	 * The only interrupt we should see under normal conditions
	 * is the receiver interrupt. If the transmit FIFO fills up,
	 * we will also see TxAvailable interrupts.
	 */
	COMMAND(ctlr, SetRxFilter, Broadcast|MyEtherAddr);
	COMMAND(ctlr, SetReadZeroMask, AllIntr|Latch);
	COMMAND(ctlr, SetIntrMask, AllIntr|Latch);
	COMMAND(ctlr, RxEnable, 0);
	COMMAND(ctlr, TxEnable, 0);
}

static void
mode(Ctlr *ctlr, int on)
{
	if(on)
		COMMAND(ctlr, SetRxFilter, Promiscuous|Broadcast|MyEtherAddr);
	else
		COMMAND(ctlr, SetRxFilter, Broadcast|MyEtherAddr);
}

static void
receive(Ctlr *ctlr)
{
	ushort status;
	RingBuf *rb;
	int len;

	while(((status = ins(ctlr->card.io+RxStatus)) & RxEmpty) == 0){
		/*
		 * If we had an error, log it and continue
		 * without updating the ring.
		 */
		if(status & RxError){
			switch(status & RxErrMask){

			case RxErrOverrun:	/* Overrrun */
				ctlr->overflows++;
				break;

			case RxErrOversize:	/* Oversize Packet (>1514) */
			case RxErrRunt:		/* Runt Packet */
				ctlr->buffs++;
				break;
			case RxErrFraming:	/* Alignment (Framing) */
				ctlr->frames++;
				break;

			case RxErrCRC:		/* CRC */
				ctlr->crcs++;
				break;
			}
		}
		else {
			/*
			 * We have a packet. Read it into the next
			 * free ring buffer, if any.
			 * The CRC is already stripped off.
			 */
			rb = &ctlr->rb[ctlr->ri];
			if(rb->owner == Interface){
				len = (status & RxByteMask);
				rb->len = len;
	
				/*
				 * Must read len bytes padded to a
				 * doubleword. We can pick them out 16-bits
				 * at a time (can try 32-bits at a time
				 * later).
				insl(ctlr->card.io+Fifo, rb->pkt, HOWMANY(len, 4));
				 */
				inss(ctlr->card.io+Fifo, rb->pkt, HOWMANY(len, 2));

				/*
				 * Update the ring.
				 */
				rb->owner = Host;
				ctlr->ri = NEXT(ctlr->ri, ctlr->nrb);
			}
		}

		/*
		 * Discard the packet as we're done with it.
		 * Wait for discard to complete.
		 */
		COMMAND(ctlr, RxDiscard, 0);
		while(ins(ctlr->card.io+Status) & CmdInProgress)
			;
	}
}

static void
transmit(Ctlr *ctlr)
{
	RingBuf *tb;
	ushort len;

	for(tb = &ctlr->tb[ctlr->ti]; tb->owner == Interface; tb = &ctlr->tb[ctlr->ti]){
		/*
		 * If there's no room in the FIFO for this packet,
		 * set up an interrupt for when space becomes available.
		 * Output packet must be a multiple of 4 in length and
		 * we need 4 bytes for the preamble.
		 */
		len = ROUNDUP(tb->len, 4);
		if(len+4 > ins(ctlr->card.io+TxFreeBytes)){
			COMMAND(ctlr, SetTxAvailable, len);
			break;
		}

		/*
		 * There's room, copy the packet to the FIFO and free
		 * the buffer back to the host.
		 */
		outs(ctlr->card.io+Fifo, tb->len);
		outs(ctlr->card.io+Fifo, 0);
		outss(ctlr->card.io+Fifo, tb->pkt, len/2);
		tb->owner = Host;
		ctlr->ti = NEXT(ctlr->ti, ctlr->ntb);
	}
}

static ushort
getdiag(Ctlr *ctlr)
{
	ushort bytes;

	COMMAND(ctlr, SelectWindow, 4);
	bytes = ins(ctlr->card.io+FIFOdiag);
	COMMAND(ctlr, SelectWindow, 1);
	return bytes & 0xFFFF;
}

static void
interrupt(Ctlr *ctlr)
{
	ushort status, diag;
	uchar txstatus, x;

	status = ins(ctlr->card.io+Status);

	if(status & Failure){
		/*
		 * Adapter failure, try to find out why.
		 * Reset if necessary.
		 * What happens if Tx is active and we reset,
		 * need to retransmit?
		 * This probably isn't right.
		 */
		diag = getdiag(ctlr);
		print("ether509: status #%ux, diag #%ux\n", status, diag);

		if(diag & TxOverrun){
			COMMAND(ctlr, TxReset, 0);
			COMMAND(ctlr, TxEnable, 0);
		}

		if(diag & RxUnderrun){
			COMMAND(ctlr, RxReset, 0);
			attach(ctlr);
		}

		if(diag & TxOverrun)
			transmit(ctlr);

		return;
	}

	if(status & RxComplete){
		receive(ctlr);
		wakeup(&ctlr->rr);
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
			if(x = inb(ctlr->card.io+TxStatus))
				outb(ctlr->card.io+TxStatus, 0);
			txstatus |= x;
		}while(ins(ctlr->card.io+Status) & TxComplete);

		if(txstatus & (TxJabber|TxUnderrun))
			COMMAND(ctlr, TxReset, 0);
		COMMAND(ctlr, TxEnable, 0);
		ctlr->oerrs++;
	}

	if(status & (TxAvailable|TxComplete)){
		/*
		 * Reset the Tx FIFO threshold.
		 */
		if(status & TxAvailable)
			COMMAND(ctlr, AckIntr, TxAvailable);
		transmit(ctlr);
		wakeup(&ctlr->tr);
		status &= ~(TxAvailable|TxComplete);
	}

	/*
	 * Panic if there are any interrupt bits on we haven't
	 * dealt with other than Latch.
	 * Otherwise, acknowledge the interrupt.
	 */
	if(status & AllIntr)
		panic("ether509 interrupt: #%lux, #%ux\n", status, getdiag(ctlr));

	COMMAND(ctlr, AckIntr, Latch);
}

Card ether509 = {
	"3Com509",			/* ident */

	reset,				/* reset */
	0,				/* init */
	attach,				/* attach */
	mode,				/* mode */

	0,				/* read */
	0,				/* write */

	0,				/* receive */
	transmit,			/* transmit */
	interrupt,			/* interrupt */
	0,				/* watch */
	0,				/* overflow */
};
