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
 * Driver written for the 'national pcmcia ether adapter',
 * The manual says NE2000 compatible.
 * The interface appears to be pretty well described in the National
 * Semiconductor Local Area Network Databook (1992) as one of the
 * AT evaluation cards.
 *
 * The NE2000 is really just a DP8390[12] plus a data port
 * and a reset port.
 */
enum {
	Data		= 0x10,		/* offset from I/O base of data port */
	Misc		= 0x18,		/* offset from I/O base of miscellaneous port */
	Reset		= 0x1f,		/* offset from I/O base of reset port */
};


static int
reset(Ether *ether)
{
	Dp8390 *dp8390;
	ulong port;
	int i, slot;
	uchar x, *p;
	PCMmap *m;

	/*
	 * Set up the software configuration.
	 * Use defaults for port, irq, mem and size
	 * if not specified.
	 */
	if(ether->port == 0)
		ether->port = 0x320;
	if(ether->irq == 0)
		ether->irq = 10;
	if(ether->irq == 2)
		ether->irq = 9;
	if(ether->mem == 0)
		ether->mem = 0x4000;
	if(ether->size == 0)
		ether->size = 16*1024;
	port = ether->port;

	ether->private = malloc(sizeof(Dp8390));
	dp8390 = ether->private;
	dp8390->bit16 = 1;
	dp8390->ram = 0;

	dp8390->dp8390 = port;
	dp8390->data = port+Data;

	dp8390->tstart = HOWMANY(ether->mem, Dp8390BufSz);
	dp8390->pstart = dp8390->tstart + HOWMANY(sizeof(Etherpkt), Dp8390BufSz);
	dp8390->pstop = dp8390->tstart + HOWMANY(ether->size, Dp8390BufSz);

	/* see if there's a pcmcia card that looks right */
	slot = pcmspecial("PC-NIC", ether);
	if(slot < 0)
		return -1;

	/* reset ST-NIC */
	x = inb(ether->port+Reset);
	delay(2);
	outb(ether->port+Reset, x);

	/* enable interrupts */
	outb(ether->port + Misc, 1<<6);

	/* Init the (possible) chip. */
	dp8390reset(ether);

	/* set the ether address */
	m = pcmmap(slot, 0, 0x1000, 1);
	if(m == 0)
		return -1;
	p = (uchar*)(KZERO|m->isa);
	for(i = 0; i < sizeof(ether->ea); i++)
		ether->ea[i] = p[0xff0+2*i];
	pcmunmap(slot, m);
	dp8390setea(ether);

	return 0;
}

void
ether4100link(void)
{
	addethercard("NE4100", reset);
}
