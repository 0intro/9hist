#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

#include "ether.h"

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
reset(Ctlr *ctlr)
{
	int i;
	uchar ic[8], sum;

	/*
	 * Look for the interface. We read the LAN address ROM
	 * and validate the checksum - the sum of all 8 bytes
	 * should be 0xFF.
	 * While we're at it, get the (possible) interface chip
	 * registers, we'll use them to check for aliasing later.
	 */
	for(ctlr->card.io = 0x200; ctlr->card.io < 0x400; ctlr->card.io += 0x20){
		sum = 0;
		for(i = 0; i < sizeof(ctlr->ea); i++){
			ctlr->ea[i] = inb(ctlr->card.io+Lar+i);
			sum += ctlr->ea[i];
			ic[i] = inb(ctlr->card.io+i);
		}
		sum += inb(ctlr->card.io+Id);
		sum += inb(ctlr->card.io+Cksum);
		if(sum == 0xFF)
			break;
	}
	if(ctlr->card.io >= 0x400)
		return -1;

	/*
	 * Check for old, dumb 8003E, which doesn't have an interface
	 * chip. Only the msr exists out of the 1st eight registers, reads
	 * of the others just alias the 2nd eight registers, the LAN
	 * address ROM. We can check icr, irr and laar against the ethernet
	 * address read above and if they match it's an 8003E (or an
	 * 8003EBT, 8003S, 8003SH or 8003WT, we don't care), in which
	 * case the default irq gets used.
	 */
	if(memcmp(&ctlr->ea[1], &ic[1], 5) == 0){
		memset(ic, 0, sizeof(ic));
		ic[Msr] = (((ulong)ctlr->card.ramstart)>>13) & 0x3F;
		ctlr->card.watch = 0;
	}
	else{
		/*
		 * As a final sanity check for the 8013EBT, which doesn't have
		 * the 83C584 interface chip, but has 2 real registers, write Gp2 and if
		 * it reads back the same, it's not an 8013EBT.
		 */
		outb(ctlr->card.io+Gp2, 0xAA);
		inb(ctlr->card.io+Msr);				/* wiggle bus */
		if(inb(ctlr->card.io+Gp2) != 0xAA){
			memset(ic, 0, sizeof(ic));
			ic[Msr] = (((ulong)ctlr->card.ramstart)>>13) & 0x3F;
			ctlr->card.watch = 0;
			ctlr->card.irq = 2;			/* special */
		}
		else
			ctlr->card.irq = intrmap[((ic[Irr]>>5) & 0x3)|(ic[Icr] & 0x4)];

		/*
		 * Check if 16-bit card.
		 * If Bit16 is read/write, then we have an 8-bit card.
		 * If Bit16 is set, we're in a 16-bit slot.
		 */
		outb(ctlr->card.io+Icr, ic[Icr]^Bit16);
		inb(ctlr->card.io+Msr);				/* wiggle bus */
		if((inb(ctlr->card.io+Icr) & Bit16) == (ic[Icr] & Bit16)){
			ctlr->card.bit16 = 1;
			ic[Icr] &= ~Bit16;
		}
		outb(ctlr->card.io+Icr, ic[Icr]);

		ic[Icr] = inb(ctlr->card.io+Icr);
		if(ctlr->card.bit16 && (ic[Icr] & Bit16) == 0)
			ctlr->card.bit16 = 0;
	}

	ctlr->card.ramstart = KZERO|((ic[Msr] & 0x3F)<<13);
	if(ctlr->card.bit16)
		ctlr->card.ramstart |= (ic[Laar] & 0x1F)<<19;
	else
		ctlr->card.ramstart |= 0x80000;

	if(ic[Icr] & (1<<3))
		ctlr->card.ramstop = 32*1024;
	else
		ctlr->card.ramstop = 8*1024;
	if(ctlr->card.bit16)
		ctlr->card.ramstop <<= 1;
	ctlr->card.ramstop += ctlr->card.ramstart;

	/*
	 * Set the DP8390 ring addresses.
	 */
	ctlr->card.dp8390 = ctlr->card.io+0x10;
	ctlr->card.pstart = HOWMANY(sizeof(Etherpkt), Dp8390BufSz);
	ctlr->card.pstop = HOWMANY(ctlr->card.ramstop-ctlr->card.ramstart, Dp8390BufSz);
	ctlr->card.tstart = 0;

	/*
	 * Enable interface RAM, set interface width.
	 */
	outb(ctlr->card.io+Msr, ic[Msr]|Menb);
	if(ctlr->card.bit16)
		outb(ctlr->card.io+Laar, ic[Laar]|L16en|M16en|ZeroWS16);

	/*
	 * Finally, init the 8390 and set the
	 * ethernet address.
	 */
	dp8390reset(ctlr);
	dp8390setea(ctlr);

	return 0;
}

static void*
read(Ctlr *ctlr, void *to, ulong from, ulong len)
{
	/*
	 * In this case, 'from' is an index into the shared memory.
	 */
	memmove(to, (void*)(ctlr->card.ramstart+from), len);
	return to;
}

static void*
write(Ctlr *ctlr, ulong to, void *from, ulong len)
{
	/*
	 * In this case, 'to' is an index into the shared memory.
	 */
	memmove((void*)(ctlr->card.ramstart+to), from, len);
	return (void*)to;
}

static void
watch(Ctlr *ctlr)
{
	uchar msr;
	int s;

	s = splhi();
	msr = inb(ctlr->card.io+Msr);
	/*
	 * If the card has reset itself,
	 * start again.
	 */
	if((msr & Menb) == 0){
		delay(100);

		dp8390reset(ctlr);
		etherinit();

		wakeup(&ctlr->tr);
		wakeup(&ctlr->rr);
	}
	splx(s);
}

/*
 * Defaults are set for the dumb 8003E
 * which can't be autoconfigured.
 */
Card ether8003 = {
	"WD8003",			/* ident */

	reset,				/* reset */
	0,				/* init */
	dp8390attach,			/* attach */
	dp8390mode,			/* mode */

	read,				/* read */
	write,				/* write */

	dp8390receive,			/* receive */
	dp8390transmit,			/* transmit */
	dp8390intr,			/* interrupt */
	watch,				/* watch */
	0,				/* overflow */

	0x280,				/* io */
	3,				/* irq */
	0,				/* bit16 */

	1,				/* ram */
	0xD0000,			/* ramstart */
	0xD0000+8*1024,			/* ramstop */
};
