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
 * Driver written for the 'Notebook Computer Ethernet LAN Adapter',
 * a plug-in to the bus-slot on the rear of the Gateway NOMAD 425DXL
 * laptop. The manual says NE2000 compatible.
 * The interface appears to be pretty well described in the National
 * Semiconductor Local Area Network Databook (1992) as one of the
 * AT evaluation cards.
 *
 * The NE2000 is really just a DP83901 plus a data port
 * and a reset port.
 */
enum {
	Data		= 0x10,		/* offset from I/O base of data port */
	Reset		= 0x18,		/* offset from I/O base of reset port */
};

static int
reset(Ctlr *ctlr)
{
	ushort buf[16];
	int i;

	/*
	 * We look for boards.
	 * We look for boards to make us barf.
	 */
	for(ctlr->card.io = 0x300; ctlr->card.io < 0x380; ctlr->card.io += 0x20){
		/*
		 * Reset the board. This is done by doing a read
		 * followed by a write to the Reset address.
		 */
		outb(ctlr->card.io+Reset, inb(ctlr->card.io+Reset));

		/*
		 * Init the (possible) chip, then use the (possible)
		 * chip to read the (possible) PROM for ethernet address
		 * and a marker byte.
		 * We could just look at the DP8390 command register after
		 * initialisation has been tried, but that wouldn't be
		 * enough, there are other ethernet boards which could
		 * match.
		 */
		ctlr->card.dp8390 = ctlr->card.io;
		ctlr->card.data = ctlr->card.io+Data;
		dp8390reset(ctlr);
		memset(buf, 0, sizeof(buf));
		dp8390read(ctlr, buf, 0, sizeof(buf));
		if((buf[0x0E] & 0xFF) == 0x57 && (buf[0x0F] & 0xFF) == 0x57)
			break;
	}
	if(ctlr->card.io >= 0x380)
		return -1;
	/*
	 * Stupid machine. We asked for shorts, we got shorts,
	 * although the PROM is a byte array.
	 * Now we can set the ethernet address.
	 */
	for(i = 0; i < sizeof(ctlr->ea); i++)
		ctlr->ea[i] = buf[i];
	dp8390setea(ctlr);

	return 0;
}

Card ether2000 = {
	"NE2000",			/* ident */

	reset,				/* reset */
	0,				/* init */
	dp8390attach,			/* attach */
	dp8390mode,			/* mode */

	dp8390read,			/* read */
	dp8390write,			/* write */

	dp8390receive,			/* receive */
	dp8390transmit,			/* transmit */
	dp8390intr,			/* interrupt */
	dp8390watch,			/* watch */
	dp8390overflow,			/* overflow */

	0x300,				/* addr */
	2,				/* irq */
	1,				/* bit16 */

	0,				/* ram */
	0x4000,				/* ramstart */
	0x4000+16*1024,			/* ramstop */

	0x300,				/* dp8390 */
	0x300+Data,			/* data */
	0,				/* software bndry */
	0x4000/Dp8390BufSz,		/* tstart */
	0x4600/Dp8390BufSz,		/* pstart */
	0x8000/Dp8390BufSz,		/* pstop */
};
