#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

#include "ether.h"

enum {					/* 83C584 Bus Interface Controller */
	Msr		= 0x00,		/* Memory Select Register */
	Icr		= 0x01,		/* Interface Configuration Register */
	Iar		= 0x02,		/* I/O Address Register */
	Bio		= 0x03,		/* BIOS ROM Address Register */
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
 */
static int
reset(Ctlr *ctlr)
{
	int i;
	uchar msr, icr, laar, irr, sum;

	/*
	 * Look for the interface. We read the LAN address ROM
	 * and validate the checksum - the sum of all 8 bytes
	 * should be 0xFF.
	 */
	for(ctlr->card.io = 0x200; ctlr->card.io < 0x400; ctlr->card.io += 0x20){
		sum = 0;
		for(i = 0; i < sizeof(ctlr->ea); i++){
			ctlr->ea[i] = inb(ctlr->card.io+Lar+i);
			sum += ctlr->ea[i];
		}
		sum += inb(ctlr->card.io+Id);
		sum += inb(ctlr->card.io+Cksum);
		if(sum == 0xFF)
			break;
	}
	if(ctlr->card.io >= 0x400)
		return -1;

	/*
	 * Found it, reset it.
	 * Be careful to preserve the Msr address bits,
	 * they don't get reloaded from the EEPROM on reset.
	 */
	msr = inb(ctlr->card.io+Msr);
	outb(ctlr->card.io+Msr, Rst|msr);
	delay(1);
	outb(ctlr->card.io+Msr, msr);
	delay(2);

	/*
	 * Check for old, dumb 8003E, which doesn't have an interface
	 * chip. Only the msr exists out of the 1st eight registers, reads
	 * of the others just alias the 2nd eight registers, the LAN
	 * address ROM. We can check icr, irr and laar against the ethernet
	 * address read above and if they match it's an 8003E (or an
	 * 8003EBT, 8003S, 8003SH or 8003WT, we don't care), in which
	 * case the default irq gets used.
	 */
	msr = inb(ctlr->card.io+Msr);
	icr = inb(ctlr->card.io+Icr);
	laar = inb(ctlr->card.io+Laar);
	irr = inb(ctlr->card.io+Irr);
	if(icr != ctlr->ea[1] || irr != ctlr->ea[4] || laar != ctlr->ea[5])
		ctlr->card.irq = intrmap[((irr>>5) & 0x3)|(icr & 0x4)];
	else {
		msr = (((ulong)ctlr->card.ramstart)>>13) & 0x3F;
		icr = laar = 0;
		ctlr->card.watch = 0;
	}

	/*
	 * Set up the bus-size, RAM address and RAM size
	 * from the info in the configuration registers.
	 */
	ctlr->card.bit16 = icr & 0x1;

	ctlr->card.ram = 1;
	ctlr->card.ramstart = KZERO|((msr & 0x3F)<<13);
	if(ctlr->card.bit16)
		ctlr->card.ramstart |= (laar & 0x1F)<<19;
	else
		ctlr->card.ramstart |= 0x80000;

	if(icr & (1<<3))
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
	outb(ctlr->card.io+Msr, Menb|msr);
	if(ctlr->card.bit16)
		outb(ctlr->card.io+Laar, laar|L16en|M16en|ZeroWS16);

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
