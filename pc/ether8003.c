#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"

#include "etherif.h"

/*
 * Western Digital/Standard Microsystems Corporation cards (WD80[01]3).
 * Configuration code based on that provided by SMC.
 */
enum {					/* 83C584 Bus Interface Controller */
	Msr		= 0x00,		/* Memory Select Register */
	Icr		= 0x01,		/* Interface Configuration Register */
	Iar		= 0x02,		/* I/O Address Register */
	Bio		= 0x03,		/* BIOS ROM Address Register */
	Ear		= 0x03,		/* EEROM Address Register (shared with Bio) */
	Irr		= 0x04,		/* Interrupt Request Register */
	Laar		= 0x05,		/* LA Address Register */
	Ijr		= 0x06,		/* Initialisation Jumpers */
	Gp2		= 0x07,		/* General Purpose Data Register */
	Lar		= 0x08,		/* LAN Address Registers */
	Id		= 0x0E,		/* Card ID byte */
	Cksum		= 0x0F,		/* Checksum */
};

enum {					/* Msr */
	Rst		= 0x80,		/* software reset */
	Menb		= 0x40,		/* memory enable */
};

enum {					/* Icr */
	Bit16		= 0x01,		/* 16-bit bus */
	Other		= 0x02,		/* other register access */
	Ir2		= 0x04,		/* IR2 */
	Msz		= 0x08,		/* SRAM size */
	Rla		= 0x10,		/* recall LAN address */
	Rx7		= 0x20,		/* recall all but I/O and LAN address */
	Rio		= 0x40,		/* recall I/O address from EEROM */
	Sto		= 0x80,		/* non-volatile EEROM store */
};

enum {					/* Laar */
	ZeroWS16	= (1<<5),	/* zero wait states for 16-bit ops */
	L16en		= (1<<6),	/* enable 16-bit LAN operation */
	M16en		= (1<<7),	/* enable 16-bit memory access */
};

/*
 * Mapping from configuration bits to interrupt level.
 */
static int intrmap[] = {
	9, 3, 5, 7, 10, 11, 15, 4,
};

/*
 * Get configuration parameters, enable memory.
 * There are opportunities here for buckets of code.
 * We'll try to resist.
 */
static int
reset(Ether *ether)
{
	int i;
	uchar ea[Eaddrlen], ic[8], sum;
	ulong port;
	Dp8390 *dp8390;

	/*
	 * Set up the software configuration.
	 * Use defaults for port, irq, mem and size if not specified.
	 * Defaults are set for the dumb 8003E which can't be
	 * autoconfigured.
	 */
	if(ether->port == 0)
		ether->port = 0x280;
	if(ether->irq == 0)
		ether->irq = 3;
	if(ether->mem == 0)
		ether->mem = 0xD0000;
	if(ether->size == 0)
		ether->size = 8*1024;

	/*
	 * Look for the interface. We read the LAN address ROM
	 * and validate the checksum - the sum of all 8 bytes
	 * should be 0xFF.
	 * While we're at it, get the (possible) interface chip
	 * registers, we'll use them to check for aliasing later.
	 */
	port = ether->port;
	sum = 0;
	for(i = 0; i < sizeof(ea); i++){
		ea[i] = inb(port+Lar+i);
		sum += ea[i];
		ic[i] = inb(port+i);
	}
	sum += inb(port+Id);
	sum += inb(port+Cksum);
	if(sum != 0xFF)
		return -1;

	ether->private = malloc(sizeof(Dp8390));
	dp8390 = ether->private;
	dp8390->ram = 1;

	/*
	 * Check for old, dumb 8003E, which doesn't have an interface
	 * chip. Only the msr exists out of the 1st eight registers, reads
	 * of the others just alias the 2nd eight registers, the LAN
	 * address ROM. We can check icr, irr and laar against the ethernet
	 * address read above and if they match it's an 8003E (or an
	 * 8003EBT, 8003S, 8003SH or 8003WT, we don't care), in which
	 * case the default irq gets used.
	 */
	if(memcmp(&ether->ea[1], &ic[1], 5) == 0){
		memset(ic, 0, sizeof(ic));
		ic[Msr] = (((ulong)ether->mem)>>13) & 0x3F;
	}
	else{
		/*
		 * As a final sanity check for the 8013EBT, which doesn't have
		 * the 83C584 interface chip, but has 2 real registers, write Gp2 and if
		 * it reads back the same, it's not an 8013EBT.
		 */
		outb(port+Gp2, 0xAA);
		inb(port+Msr);				/* wiggle bus */
		if(inb(port+Gp2) != 0xAA){
			memset(ic, 0, sizeof(ic));
			ic[Msr] = (((ulong)ether->mem)>>13) & 0x3F;
		}
		else
			ether->irq = intrmap[((ic[Irr]>>5) & 0x3)|(ic[Icr] & 0x4)];

		/*
		 * Check if 16-bit card.
		 * If Bit16 is read/write, then we have an 8-bit card.
		 * If Bit16 is set, we're in a 16-bit slot.
		 */
		outb(port+Icr, ic[Icr]^Bit16);
		inb(port+Msr);				/* wiggle bus */
		if((inb(port+Icr) & Bit16) == (ic[Icr] & Bit16)){
			dp8390->bit16 = 1;
			ic[Icr] &= ~Bit16;
		}
		outb(port+Icr, ic[Icr]);

		if(dp8390->bit16 && (inb(port+Icr) & Bit16) == 0)
			dp8390->bit16 = 0;
	}

	ether->mem = KZERO|((ic[Msr] & 0x3F)<<13);
	if(dp8390->bit16)
		ether->mem |= (ic[Laar] & 0x1F)<<19;
	else
		ether->mem |= 0x80000;

	if(ic[Icr] & (1<<3))
		ether->size = 32*1024;
	if(dp8390->bit16)
		ether->size <<= 1;

	/*
	 * Set the DP8390 ring addresses.
	 */
	dp8390->dp8390 = port+0x10;
	dp8390->tstart = 0;
	dp8390->pstart = HOWMANY(sizeof(Etherpkt), Dp8390BufSz);
	dp8390->pstop = HOWMANY(ether->size, Dp8390BufSz);

	/*
	 * Enable interface RAM, set interface width.
	 */
	outb(port+Msr, ic[Msr]|Menb);
	if(dp8390->bit16)
		outb(port+Laar, ic[Laar]|L16en|M16en|ZeroWS16);

	/*
	 * Finally, init the 8390 and set the
	 * ethernet address.
	 */
	dp8390reset(ether);
	if((ether->ea[0]|ether->ea[1]|ether->ea[2]|ether->ea[3]|ether->ea[4]|ether->ea[5]) == 0){
		for(i = 0; i < sizeof(ether->ea); i++)
			ether->ea[i] = ea[i];
	}
	dp8390setea(ether);

	if(getisa(ether->mem, ether->size, 0) == 0)
		panic("ether8003: 0x%lux reused", ether->mem);

	return 0;
}

void
ether8003link(void)
{
	addethercard("WD8003", reset);
}
