#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

#define NEXT(x, l)	(((x)+1)%(l))
#define OFFSETOF(t, m)	((unsigned)&(((t*)0)->m))
#define	HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))

enum {
	Nrb		= 16,		/* software receive buffers */
	Ntb		= 4,		/* software transmit buffers */
};

enum {
	IDport		= 0x0100,	/* anywhere between 0x0100 and 0x01F0 */

					/* Commands */
	SelectWindow	= 0x01,		/* SelectWindow command */
	StartCoax	= 0x02,		/* Start Coaxial Transceiver */
	RxDisable	= 0x03,		/* RX Disable */
	RxEnable	= 0x04,		/* RX Enable */
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
};

#define COMMAND(hw, cmd, a)	outs(hw->addr+Command, ((cmd)<<11)|(a))

/*
 *  Write two 0 bytes to identify the IDport and them reset the
 *  ID sequence.  Then send the ID sequenced to the card to get
 *  the card it into command state.
 */
void
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
 * get configuration parameters
 */
static int
reset(EtherCtlr *cp)
{
	EtherHw *hw = cp->hw;
	int i, ea;
	ushort x, acr;

	cp->rb = xspanalloc(sizeof(EtherBuf)*Nrb, BY2PG, 0);
	cp->nrb = Nrb;
	cp->tb = xspanalloc(sizeof(EtherBuf)*Ntb, BY2PG, 0);
	cp->ntb = Ntb;

	/*
	 * Do the little configuration dance. We only look
	 * at the first board that responds, if we ever have more
	 * than one we'll need to modify this sequence.
	 *
	 * 2. get to cammand state, reset, then return to command state
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
	hw->addr = (acr & 0x1F)*0x10 + 0x200;
	outb(hw->addr+ConfigControl, 0x01);

	/*
	 * Read the IRQ from the Resource Configuration Register
	 * and the ethernet address from the EEPROM.
	 * The EEPROM command is 8bits, the lower 6 bits being
	 * the address offset.
	 */
	hw->irq = (ins(hw->addr+ResourceConfig)>>12) & 0x0F;
	for(ea = 0, i = 0; i < 3; i++, ea += 2){
		while(ins(hw->addr+EEPROMcmd) & 0x8000)
			;
		outs(hw->addr+EEPROMcmd, (2<<6)|i);
		while(ins(hw->addr+EEPROMcmd) & 0x8000)
			;
		x = ins(hw->addr+EEPROMdata);
		cp->ea[ea] = (x>>8) & 0xFF;
		cp->ea[ea+1] = x & 0xFF;
	}

	/*
	 * Finished with window 0. Now set the ethernet address
	 * in window 2.
	 * Commands have the format 'CCCCCAAAAAAAAAAA' where C
	 * is a bit in the command and A is a bit in the argument.
	 */
	COMMAND(hw, SelectWindow, 2);
	for(i = 0; i < 6; i++)
		outb(hw->addr+i, cp->ea[i]);

	/*
	 * Finished with window 2.
	 * Set window 1 for normal operation.
	 */
	COMMAND(hw, SelectWindow, 1);

	/*
	 * If we have a 10BASE2 transceiver, start the DC-DC
	 * converter. Wait > 800 microseconds.
	 */
	if(((acr>>14) & 0x03) == 0x03){
		COMMAND(hw, StartCoax, 0);
		delay(1);
	}

	print("3C509 I/O addr %lux irq %d:", hw->addr, hw->irq);
	for(i = 0; i < sizeof(cp->ea); i++)
		print(" %2.2ux", cp->ea[i]);
	print("\n");

	return 0;
}

static void
mode(EtherCtlr *cp, int on)
{
	EtherHw *hw = cp->hw;

	qlock(cp);
	if(on){
		cp->prom++;
		if(cp->prom == 1)
			COMMAND(hw, SetRxFilter, Promiscuous|Broadcast|MyEtherAddr);
	}
	else{
		cp->prom--;
		if(cp->prom == 0)
			COMMAND(hw, SetRxFilter, Broadcast|MyEtherAddr);
	}
	qunlock(cp);
}

static void
online(EtherCtlr *cp, int on)
{
	EtherHw *hw = cp->hw;

	USED(on);					/* BUG */
	/*
	 * Set the receiver packet filter for our own and
	 * and broadcast addresses, set the interrupt masks
	 * for all interrupts, and enable the receiver and transmitter.
	 * The only interrupt we should see under normal conditions
	 * is the receiver interrupt. If the transmit FIFO fills up,
	 * we will also see TxAvailable interrupts.
	 */
	COMMAND(hw, SetRxFilter, Broadcast|MyEtherAddr);
	COMMAND(hw, SetReadZeroMask, AllIntr|Latch);
	COMMAND(hw, SetIntrMask, AllIntr|Latch);
	COMMAND(hw, RxEnable, 0);
	COMMAND(hw, TxEnable, 0);
}


static int
getdiag(EtherHw *hw)
{
	int bytes;

	COMMAND(hw, SelectWindow, 4);
	bytes = ins(hw->addr+0x04);
	COMMAND(hw, SelectWindow, 1);
	return bytes & 0xFFFF;
}

static void
receive(EtherCtlr *cp)
{
	EtherHw *hw = cp->hw;
	ushort status;
	EtherBuf *rb;
	int len;

	while(((status = ins(hw->addr+RxStatus)) & RxEmpty) == 0){

		/*
		 * If we had an error, log it and continue
		 * without updating the ring.
		 */
		if(status & RxError){
			switch(status & RxErrMask){

			case RxErrOverrun:	/* Overrrun */
				cp->overflows++;
				break;

			case RxErrOversize:	/* Oversize Packet (>1514) */
			case RxErrRunt:		/* Runt Packet */
				cp->buffs++;
				break;
			case RxErrFraming:	/* Alignment (Framing) */
				cp->frames++;
				break;

			case RxErrCRC:		/* CRC */
				cp->crcs++;
				break;
			}
		}
		else {
			/*
			 * We have a packet. Read it into the next
			 * free ring buffer, if any.
			 * The CRC is already stripped off.
			 */
			rb = &cp->rb[cp->ri];
			if(rb->owner == Interface){
				len = (status & RxByteMask);
				rb->len = len;
	
				/*
				 * Must read len bytes padded to a
				 * doubleword. We can pick them out 16-bits
				 * at a time (can try 32-bits at a time
				 * later).
				insl(hw->addr+Fifo, rb->pkt, HOWMANY(len, 4));
				 */
				inss(hw->addr+Fifo, rb->pkt, HOWMANY(len, 2));

				/*
				 * Update the ring.
				 */
				rb->owner = Host;
				cp->ri = NEXT(cp->ri, cp->nrb);
			}
		}
		/*
		 * Discard the packet as we're done with it.
		 * Wait for discard to complete.
		 */
		COMMAND(hw, RxDiscard, 0);
		while(ins(hw->addr+Status) & CmdInProgress)
			;
	}
}

static void
transmit(EtherCtlr *cp)
{
	EtherHw *hw = cp->hw;
	EtherBuf *tb;
	int s;
	ushort len;

	s = splhi();
	for(tb = &cp->tb[cp->ti]; tb->owner == Interface; tb = &cp->tb[cp->ti]){

		/*
		 * If there's no room in the FIFO for this packet,
		 * set up an interrupt for when space becomes available.
		 */
		if(tb->len > ins(hw->addr+TxFreeBytes)){
			COMMAND(hw, SetTxAvailable, tb->len);
			break;
		}

		/*
		 * There's room, copy the packet to the FIFO and free
		 * the buffer back to the host.
		 * Output packet must be a multiple of 4 in length.
		 */
		len = ROUNDUP(tb->len, 4)/2;
		outs(hw->addr+Fifo, tb->len);
		outs(hw->addr+Fifo, 0);
		outss(hw->addr+Fifo, tb->pkt, len);
		tb->owner = Host;
		cp->ti = NEXT(cp->ti, cp->ntb);
	}
	splx(s);
}

static void
interrupt(EtherCtlr *cp)
{
	EtherHw *hw = cp->hw;
	ushort status;
	uchar txstatus, x;

	status = ins(hw->addr+Status);

	if(status & RxComplete){
		(*cp->hw->receive)(cp);
		wakeup(&cp->rr);
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
			if(x = inb(hw->addr+TxStatus))
				outb(hw->addr+TxStatus, 0);
			txstatus |= x;
		}while(ins(hw->addr+Status) & TxComplete);
		if(txstatus & (TxJabber|TxUnderrun))
			COMMAND(hw, TxReset, 0);
		COMMAND(hw, TxEnable, 0);
		cp->oerrs++;
	}

	if(status & (TxAvailable|TxComplete)){
		/*
		 * Reset the Tx FIFO threshold.
		 */
		if(status & TxAvailable)
			COMMAND(hw, AckIntr, TxAvailable);
		(*cp->hw->transmit)(cp);
		wakeup(&cp->tr);
		status &= ~(TxAvailable|TxComplete);
	}

	/*
	 * Panic if there are any interrupt bits on we haven't
	 * dealt with other than Latch.
	 */
	if(status & AllIntr)
		panic("ether509 interrupt: #%lux, #%ux\n", status, getdiag(hw));

	/*
	 * Acknowledge the interrupt.
	 */
	COMMAND(hw, AckIntr, Latch);
}

EtherHw ether509 = {
	reset,
	0,					/* init */
	mode,
	online,
	receive,
	transmit,
	interrupt,
	0,					/* tweak */
	0,					/* I/O base address */
};
