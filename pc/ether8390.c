/*
 * National Semiconductor DP8390
 * and SMC 83C90
 * Network Interface Controller.
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

extern int slowinb(ulong);
extern void slowoutb(ulong, uchar);

enum {
	Cr		= 0x00,		/* command register, all pages */

	Stp		= 0x01,		/* stop */
	Sta		= 0x02,		/* start */
	Txp		= 0x04,		/* transmit packet */
	RDMAread	= (1<<3),	/* remote DMA read */
	RDMAwrite	= (2<<3),	/* remote DMA write */
	RDMAsend	= (3<<3),	/* remote DMA send packet */
	RDMAabort	= (4<<3),	/* abort/complete remote DMA */
	Ps0		= 0x40,		/* page select */
	Ps1		= 0x80,		/* page select */
	Page0		= 0x00,
	Page1		= Ps0,
	Page2		= Ps1,
};

enum {					/* Page 0, read */
	Clda0		= 0x01,		/* current local DMA address 0 */
	Clda1		= 0x02,		/* current local DMA address 1 */
	Bnry		= 0x03,		/* boundary pointer (R/W) */
	Tsr		= 0x04,		/* transmit status register */
	Ncr		= 0x05,		/* number of collisions register */
	Fifo		= 0x06,		/* FIFO */
	Isr		= 0x07,		/* interrupt status register (R/W) */
	Crda0		= 0x08,		/* current remote DMA address 0 */
	Crda1		= 0x09,		/* current remote DMA address 1 */
	Rsr		= 0x0C,		/* receive status register */
	Cntr0		= 0x0D,		/* frame alignment errors */
	Cntr1		= 0x0E,		/* CRC errors */
	Cntr2		= 0x0F,		/* missed packet errors */
};

enum {					/* Page 0, write */
	Pstart		= 0x01,		/* page start register */
	Pstop		= 0x02,		/* page stop register */
	Tpsr		= 0x04,		/* transmit page start address */
	Tbcr0		= 0x05,		/* transmit byte count register 0 */
	Tbcr1		= 0x06,		/* transmit byte count register 1 */
	Rsar0		= 0x08,		/* remote start address register 0 */
	Rsar1		= 0x09,		/* remote start address register 1 */
	Rbcr0		= 0x0A,		/* remote byte count register 0 */
	Rbcr1		= 0x0B,		/* remote byte count register 1 */
	Rcr		= 0x0C,		/* receive configuration register */
	Tcr		= 0x0D,		/* transmit configuration register */
	Dcr		= 0x0E,		/* data configuration register */
	Imr		= 0x0F,		/* interrupt mask */
};

enum {					/* Page 1, read/write */
	Par0		= 0x01,		/* physical address register 0 */
	Curr		= 0x07,		/* current page register */
	Mar0		= 0x08,		/* multicast address register 0 */
};

enum {					/* Interrupt Status Register */
	Prx		= 0x01,		/* packet received */
	Ptx		= 0x02,		/* packet transmitted */
	Rxe		= 0x04,		/* receive error */
	Txe		= 0x08,		/* transmit error */
	Ovw		= 0x10,		/* overwrite warning */
	Cnt		= 0x20,		/* counter overflow */
	Rdc		= 0x40,		/* remote DMA complete */
	Rst		= 0x80,		/* reset status */
};

enum {					/* Interrupt Mask Register */
	Prxe		= 0x01,		/* packet received interrupt enable */
	Ptxe		= 0x02,		/* packet transmitted interrupt enable */
	Rxee		= 0x04,		/* receive error interrupt enable */
	Txee		= 0x08,		/* transmit error interrupt enable */
	Ovwe		= 0x10,		/* overwrite warning interrupt enable */
	Cnte		= 0x20,		/* counter overflow interrupt enable */
	Rdce		= 0x40,		/* DMA complete interrupt enable */
};

enum {					/* Data Configuration register */
	Wts		= 0x01,		/* word transfer select */
	Bos		= 0x02,		/* byte order select */
	Las		= 0x04,		/* long address select */
	Ls		= 0x08,		/* loopback select */
	Arm		= 0x10,		/* auto-initialise remote */
	Ft1		= (0x00<<5),	/* FIFO threshhold select 1 byte/word */
	Ft2		= (0x01<<5),	/* FIFO threshhold select 2 bytes/words */
	Ft4		= (0x02<<5),	/* FIFO threshhold select 4 bytes/words */
	Ft6		= (0x03<<5),	/* FIFO threshhold select 6 bytes/words */
};

enum {					/* Transmit Configuration Register */
	Crc		= 0x01,		/* inhibit CRC */
	Lb		= 0x02,		/* internal loopback */
	Atd		= 0x08,		/* auto transmit disable */
	Ofst		= 0x10,		/* collision offset enable */
};

enum {					/* Transmit Status Register */
	Ptxok		= 0x01,		/* packet transmitted */
	Col		= 0x04,		/* transmit collided */
	Abt		= 0x08,		/* tranmit aborted */
	Crs		= 0x10,		/* carrier sense lost */
	Fu		= 0x20,		/* FIFO underrun */
	Cdh		= 0x40,		/* CD heartbeat */
	Owc		= 0x80,		/* out of window collision */
};

enum {					/* Receive Configuration Register */
	Sep		= 0x01,		/* save errored packets */
	Ar		= 0x02,		/* accept runt packets */
	Ab		= 0x04,		/* accept broadcast */
	Am		= 0x08,		/* accept multicast */
	Pro		= 0x10,		/* promiscuous physical */
	Mon		= 0x20,		/* monitor mode */
};

enum {					/* Receive Status Register */
	Prxok		= 0x01,		/* packet received intact */
	Crce		= 0x02,		/* CRC error */
	Fae		= 0x04,		/* frame alignment error */
	Fo		= 0x08,		/* FIFO overrun */
	Mpa		= 0x10,		/* missed packet */
	Phy		= 0x20,		/* physical/multicast address */
	Dis		= 0x40,		/* receiver disabled */
	Dfr		= 0x80,		/* deferring */
};

typedef struct {
	uchar	status;
	uchar	next;
	uchar	len0;
	uchar	len1;
} Hdr;

static void
dp8390disable(Dp8390 *dp8390)
{
	ulong port = dp8390->dp8390;
	int timo;

	/*
	 * Stop the chip. Set the Stp bit and wait for the chip
	 * to finish whatever was on its tiny mind before it sets
	 * the Rst bit.
	 * We need the timeout because there may not be a real
	 * chip there if this is called when probing for a device
	 * at boot.
	 */
	slowoutb(port+Cr, Page0|RDMAabort|Stp);
	slowoutb(port+Rbcr0, 0);
	slowoutb(port+Rbcr1, 0);
	for(timo = 10000; (slowinb(port+Isr) & Rst) == 0 && timo; timo--)
			;
}

static void
dp8390ring(Dp8390 *dp8390)
{
	ulong port = dp8390->dp8390;

	slowoutb(port+Pstart, dp8390->pstart);
	slowoutb(port+Pstop, dp8390->pstop);
	slowoutb(port+Bnry, dp8390->pstop-1);

	slowoutb(port+Cr, Page1|RDMAabort|Stp);
	slowoutb(port+Curr, dp8390->pstart);
	slowoutb(port+Cr, Page0|RDMAabort|Stp);

	dp8390->nxtpkt = dp8390->pstart;
}

void
dp8390setea(Ether *ether)
{
	ulong port = ((Dp8390*)ether->ctlr)->dp8390;
	uchar cr;
	int i;

	/*
	 * Set the ethernet address into the chip.
	 * Take care to restore the command register
	 * afterwards. We don't care about multicast
	 * addresses as we never set the multicast
	 * enable.
	 */
	cr = slowinb(port+Cr) & ~Txp;
	slowoutb(port+Cr, Page1|(~(Ps1|Ps0) & cr));
	for(i = 0; i < sizeof(ether->ea); i++)
		slowoutb(port+Par0+i, ether->ea[i]);
	slowoutb(port+Cr, cr);
}

void
dp8390getea(Ether *ether)
{
	ulong port = ((Dp8390*)ether->ctlr)->dp8390;
	uchar cr;
	int i;

	/*
	 * Get the ethernet address from the chip.
	 * Take care to restore the command register
	 * afterwards. We don't care about multicast
	 * addresses as we never set the multicast
	 * enable.
	 */
	cr = slowinb(port+Cr) & ~Txp;
	slowoutb(port+Cr, Page1|(~(Ps1|Ps0) & cr));
	for(i = 0; i < sizeof(ether->ea); i++)
		ether->ea[i] = slowinb(port+Par0+i);
	slowoutb(port+Cr, cr);
}

void*
dp8390read(Dp8390 *dp8390, void *to, ulong from, ulong len)
{
	ulong port = dp8390->dp8390;
	uchar cr;
	int timo;

	/*
	 * Read some data at offset 'from' in the card's memory
	 * using the DP8390 remote DMA facility, and place it at
	 * 'to' in main memory, via the I/O data port.
	 */
	cr = slowinb(port+Cr) & ~Txp;
	slowoutb(port+Cr, Page0|RDMAabort|Sta);
	slowoutb(port+Isr, Rdc);

	/*
	 * Set up the remote DMA address and count.
	 */
	if(dp8390->bit16)
		len = ROUNDUP(len, 2);
	slowoutb(port+Rbcr0, len & 0xFF);
	slowoutb(port+Rbcr1, (len>>8) & 0xFF);
	slowoutb(port+Rsar0, from & 0xFF);
	slowoutb(port+Rsar1, (from>>8) & 0xFF);

	/*
	 * Start the remote DMA read and suck the data
	 * out of the I/O port.
	 */
	slowoutb(port+Cr, Page0|RDMAread|Sta);
	if(dp8390->bit16)
		inss(dp8390->data, to, len/2);
	else
		insb(dp8390->data, to, len);

	/*
	 * Wait for the remote DMA to complete. The timeout
	 * is necessary because we may call this routine on
	 * a non-existent chip during initialisation and, due
	 * to the miracles of the bus, we could get this far
	 * and still be talking to a slot full of nothing.
	 */
	for(timo = 10000; (slowinb(port+Isr) & Rdc) == 0 && timo; timo--)
			;

	slowoutb(port+Isr, Rdc);
	slowoutb(port+Cr, cr);
	return to;
}

void*
dp8390write(Dp8390 *dp8390, ulong to, void *from, ulong len)
{
	ulong port = dp8390->dp8390;
	ulong crda;
	uchar cr;
	int s, tries;

	/*
	 * Keep out interrupts since reading and writing
	 * use the same DMA engine.
	 */
	s = splhi();

	/*
	 * Write some data to offset 'to' in the card's memory
	 * using the DP8390 remote DMA facility, reading it at
	 * 'from' in main memory, via the I/O data port.
	 */
	cr = slowinb(port+Cr) & ~Txp;
	slowoutb(port+Cr, Page0|RDMAabort|Sta);
	slowoutb(port+Isr, Rdc);

	if(dp8390->bit16)
		len = ROUNDUP(len, 2);

	/*
	 * Set up the remote DMA address and count.
	 * This is straight from the DP8390[12D] datasheet, hence
	 * the initial set up for read.
	 */
	crda = to-1-dp8390->bit16;
	slowoutb(port+Rbcr0, (len+1+dp8390->bit16) & 0xFF);
	slowoutb(port+Rbcr1, ((len+1+dp8390->bit16)>>8) & 0xFF);
	slowoutb(port+Rsar0, crda & 0xFF);
	slowoutb(port+Rsar1, (crda>>8) & 0xFF);
	slowoutb(port+Cr, Page0|RDMAread|Sta);

	for(;;){
		crda = slowinb(port+Crda0);
		crda |= slowinb(port+Crda1)<<8;
		if(crda == to){
			/*
			 * Start the remote DMA write and make sure
			 * the registers are correct.
			 */
			slowoutb(port+Cr, Page0|RDMAwrite|Sta);

			crda = slowinb(port+Crda0);
			crda |= slowinb(port+Crda1)<<8;
			if(crda != to)
				panic("crda write %d to %d\n", crda, to);

			break;
		}
	}

	/*
	 * Pump the data into the I/O port.
	 */
	if(dp8390->bit16)
		outss(dp8390->data, from, len/2);
	else
		outsb(dp8390->data, from, len);

	/*
	 * Wait for the remote DMA to finish. It should
	 * be almost immediate.
	 */
	tries = 0;
	while((slowinb(port+Isr) & Rdc) == 0){
		if(tries++ >= 100000){
			print("dp8390write dma timed out\n");
			break;
		}
	}

	slowoutb(port+Isr, Rdc);
	slowoutb(port+Cr, cr);
	splx(s);

	return (void*)to;
}

static uchar
getcurr(Dp8390 *dp8390)
{
	ulong port = dp8390->dp8390;
	uchar cr, curr;

	cr = slowinb(port+Cr) & ~Txp;
	slowoutb(port+Cr, Page1|(~(Ps1|Ps0) & cr));
	curr = slowinb(port+Curr);
	slowoutb(port+Cr, cr);
	return curr;
}

static void
receive(Ether *ether)
{
	Dp8390 *dp8390;
	uchar curr, *pkt;
	Hdr hdr;
	ulong port, data, len, len1;

	dp8390 = ether->ctlr;
	port = dp8390->dp8390;
	for(curr = getcurr(dp8390); dp8390->nxtpkt != curr; curr = getcurr(dp8390)){
		ether->inpackets++;

		data = dp8390->nxtpkt*Dp8390BufSz;
		if(dp8390->ram)
			memmove(&hdr, (void*)(ether->mem+data), sizeof(Hdr));
		else
			dp8390read(dp8390, &hdr, data, sizeof(Hdr));

		/*
		 * Don't believe the upper byte count, work it
		 * out from the software next-page pointer and
		 * the current next-page pointer.
		 */
		if(hdr.next > dp8390->nxtpkt)
			len1 = hdr.next - dp8390->nxtpkt - 1;
		else
			len1 = (dp8390->pstop-dp8390->nxtpkt) + (hdr.next-dp8390->pstart) - 1;
		if(hdr.len0 > (Dp8390BufSz-sizeof(Hdr)))
			len1--;

		len = ((len1<<8)|hdr.len0)-4;

		/*
		 * Chip is badly scrogged, reinitialise the ring.
		 */
		if(hdr.next < dp8390->pstart || hdr.next >= dp8390->pstop
		  || len < 60 || len > sizeof(Etherpkt)){
			print("dp8390: H#%2.2ux#%2.2ux#%2.2ux#%2.2ux,%d\n",
				hdr.status, hdr.next, hdr.len0, hdr.len1, len);
			slowoutb(port+Cr, Page0|RDMAabort|Stp);
			dp8390ring(dp8390);
			slowoutb(port+Cr, Page0|RDMAabort|Sta);
			return;
		}

		/*
		 * If it's a good packet read it in to the software buffer.
		 * If the packet wraps round the hardware ring, read it in two pieces.
		 *
		 * We could conceivably remove the copy into rpkt here by wrapping
		 * this up with the etherrloop code.
		 */
		if((hdr.status & (Fo|Fae|Crce|Prxok)) == Prxok){
			pkt = (uchar*)&ether->rpkt;
			data += sizeof(Hdr);
			len1 = len;

			if((data+len1) >= dp8390->pstop*Dp8390BufSz){
				ulong count = dp8390->pstop*Dp8390BufSz - data;

				if(dp8390->ram)
					memmove(pkt, (void*)(ether->mem+data), count);
				else
					dp8390read(dp8390, pkt, data, count);
				pkt += count;
				data = dp8390->pstart*Dp8390BufSz;
				len1 -= count;
			}
			if(len1){
				if(dp8390->ram)
					memmove(pkt, (void*)(ether->mem+data), len1);
				else
					dp8390read(dp8390, pkt, data, len1);
			}

			/*
			 * Copy the packet to whoever wants it.
			 */
			etherrloop(ether, &ether->rpkt, len);
		}

		/*
		 * Finished with this packet, update the
		 * hardware and software ring pointers.
		 */
		dp8390->nxtpkt = hdr.next;

		hdr.next--;
		if(hdr.next < dp8390->pstart)
			hdr.next = dp8390->pstop-1;
		slowoutb(port+Bnry, hdr.next);
	}
}

static int
istxavail(void *arg)
{
	return ((Ether*)arg)->tlen == 0;
}

static long
write(Ether *ether, void *buf, long len)
{
	Dp8390 *dp8390;
	ulong port;
	Etherpkt *pkt;

	dp8390 = ether->ctlr;
	port = dp8390->dp8390;

	tsleep(&ether->tr, istxavail, ether, 10000);
	if(ether->tlen){
		print("dp8390: transmitter jammed\n");
		return 0;
	}
	ether->tlen = len;

	/*
	 * If it's a shared-memory interface, copy the packet
	 * directly to the shared-memory area. Otherwise, copy
	 * it to a staging buffer so the I/O-port write can be
	 * done in one.
	 */
	if(dp8390->ram)
		pkt = (Etherpkt*)(ether->mem+dp8390->tstart*Dp8390BufSz);
	else
		pkt = &ether->tpkt;
	memmove(pkt, buf, len);

	/*
	 * Give the packet a source address and make sure it
	 * is of minimum length.
	 */
	memmove(pkt->s, ether->ea, sizeof(ether->ea));
	if(len < ETHERMINTU){
		memset(pkt->d+len, 0, ETHERMINTU-len);
		len = ETHERMINTU;
	}

	if(dp8390->ram == 0)
		dp8390write(dp8390, dp8390->tstart*Dp8390BufSz, pkt, len);

	slowoutb(port+Tbcr0, len & 0xFF);
	slowoutb(port+Tbcr1, (len>>8) & 0xFF);
	slowoutb(port+Cr, Page0|RDMAabort|Txp|Sta);

	return len;
}

static void
overflow(Ether *ether)
{
	Dp8390 *dp8390;
	ulong port;
	uchar txp;
	int resend;

	dp8390 = ether->ctlr;
	port = dp8390->dp8390;

	/*
	 * The following procedure is taken from the DP8390[12D] datasheet,
	 * it seems pretty adamant that this is what has to be done.
	 */
	txp = slowinb(port+Cr) & Txp;
	slowoutb(port+Cr, Page0|RDMAabort|Stp);
	delay(2);
	slowoutb(port+Rbcr0, 0);
	slowoutb(port+Rbcr1, 0);

	resend = 0;
	if(txp && (slowinb(port+Isr) & (Txe|Ptx)) == 0)
		resend = 1;

	slowoutb(port+Tcr, Lb);
	slowoutb(port+Cr, Page0|RDMAabort|Sta);
	receive(ether);
	slowoutb(port+Isr, Ovw);
	slowoutb(port+Tcr, 0);

	if(resend)
		slowoutb(port+Cr, Page0|RDMAabort|Txp|Sta);
}

static void
interrupt(Ureg *ur, void *arg)
{
	Ether *ether;
	Dp8390 *dp8390;
	ulong port;
	uchar isr, r;

	USED(ur);

	ether = arg;
	dp8390 = ether->ctlr;
	port = dp8390->dp8390;

	/*
	 * While there is something of interest,
	 * clear all the interrupts and process.
	 */
	slowoutb(port+Imr, 0x00);
	while(isr = slowinb(port+Isr)){

		if(isr & Ovw){
			overflow(ether);
			slowoutb(port+Isr, Ovw);
			ether->overflows++;
		}

		/*
		 * We have received packets.
		 * Take a spin round the ring. and
		 */
		if(isr & (Rxe|Prx)){
			receive(ether);
			slowoutb(port+Isr, Rxe|Prx);
		}

		/*
		 * A packet completed transmission, successfully or
		 * not. Start transmission on the next buffered packet,
		 * and wake the output routine.
		 */
		if(isr & (Txe|Ptx)){
			r = slowinb(port+Tsr);
			if(isr & Txe){
				if((r & (Cdh|Fu|Crs|Abt)))
					print("dp8390: Tsr#%2.2ux\n", r);
				ether->oerrs++;
			}

			slowoutb(port+Isr, Txe|Ptx);

			if(isr & Ptx)
				ether->outpackets++;
			ether->tlen = 0;
			wakeup(&ether->tr);
		}

		if(isr & Cnt){
			ether->frames += slowinb(port+Cntr0);
			ether->crcs += slowinb(port+Cntr1);
			ether->buffs += slowinb(port+Cntr2);
			slowoutb(port+Isr, Cnt);
		}
	}
	slowoutb(port+Imr, Cnte|Ovwe|Txee|Rxee|Ptxe|Prxe);
}

static void
promiscuous(void *arg, int on)
{
	Dp8390 *dp8390 = ((Ether*)arg)->ctlr;

	/*
	 * Set/reset promiscuous mode.
	 */
	if(on)
		slowoutb(dp8390->dp8390+Rcr, Pro|Ab);
	else
		slowoutb(dp8390->dp8390+Rcr, Ab);
}

static void
attach(Ether *ether)
{
	Dp8390 *dp8390 = ether->ctlr;

	/*
	 * Enable the chip for transmit/receive.
	 * The init routine leaves the chip in monitor
	 * mode. Clear the missed-packet counter, it
	 * increments while in monitor mode.
	 */
	slowoutb(dp8390->dp8390+Rcr, Ab);
	slowinb(dp8390->dp8390+Cntr2);
}

int
dp8390reset(Ether *ether)
{
	Dp8390 *dp8390;
	ulong port;

	dp8390 = ether->ctlr;
	port = dp8390->dp8390;

	/*
	 * This is the initialisation procedure described
	 * as 'mandatory' in the datasheet, with references
	 * to the 3Com503 technical reference manual.
	 */ 
	dp8390disable(dp8390);
	if(dp8390->bit16)
		slowoutb(port+Dcr, Ft4|Ls|Wts);
	else
		slowoutb(port+Dcr, Ft4|Ls);

	slowoutb(port+Rbcr0, 0);
	slowoutb(port+Rbcr1, 0);

	slowoutb(port+Tcr, 0);
	slowoutb(port+Rcr, Mon);

	/*
	 * Init the ring hardware and software ring pointers.
	 * Can't initialise ethernet address as we may not know
	 * it yet.
	 */
	dp8390ring(dp8390);
	slowoutb(port+Tpsr, dp8390->tstart);

	slowoutb(port+Isr, 0xFF);
	slowoutb(port+Imr, Cnte|Ovwe|Txee|Rxee|Ptxe|Prxe);

	/*
	 * Leave the chip initialised,
	 * but in monitor mode.
	 */
	slowoutb(port+Cr, Page0|RDMAabort|Sta);

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
