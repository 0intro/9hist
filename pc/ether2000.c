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
 * Driver written for the 'Notebook Computer Ethernet LAN Adapter',
 * a plug-in to the bus-slot on the rear of the Gateway NOMAD 425DXL
 * laptop. The manual says NE2000 compatible.
 * The interface appears to be pretty well described in the National
 * Semiconductor Local Area Network Databook (1992) as one of the
 * AT evaluation cards.
 *
 * The NE2000 is really just a DP8390[12] plus a data port
 * and a reset port.
 */
enum {
	Data		= 0x10,		/* offset from I/O base of data port */
	Reset		= 0x18,		/* offset from I/O base of reset port */
};

static int
reset(Ether *ether)
{
	ushort buf[16];
	ulong port;
	Dp8390 *dp8390;
	int i;

	/*
	 * Set up the software configuration.
	 * Use defaults for port, irq, mem and size
	 * if not specified.
	 */
	if(ether->port == 0)
		ether->port = 0x300;
	if(ether->irq == 0)
		ether->irq = 2;
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

	/*
	 * Reset the board. This is done by doing a read
	 * followed by a write to the Reset address.
	 */
	buf[0] = inb(port+Reset);
	delay(2);
	outb(port+Reset, buf[0]);
	
	/*
	 * Init the (possible) chip, then use the (possible)
	 * chip to read the (possible) PROM for ethernet address
	 * and a marker byte.
	 * We could just look at the DP8390 command register after
	 * initialisation has been tried, but that wouldn't be
	 * enough, there are other ethernet boards which could
	 * match.
	 */
	dp8390reset(ether);
	memset(buf, 0, sizeof(buf));
	dp8390read(dp8390, buf, 0, sizeof(buf));
	if((buf[0x0E] & 0xFF) != 0x57 || (buf[0x0F] & 0xFF) != 0x57){
		free(ether->private);
		return -1;
	}

	/*
	 * Stupid machine. We asked for shorts, we got shorts,
	 * although the PROM is a byte array.
	 * Now we can set the ethernet address.
	 */
	if((ether->ea[0]|ether->ea[1]|ether->ea[2]|ether->ea[3]|ether->ea[4]|ether->ea[5]) == 0){
		for(i = 0; i < sizeof(ether->ea); i++)
			ether->ea[i] = buf[i];
	}

	dp8390setea(ether);

	return 0;
}

void
ether2000link(void)
{
	addethercard("NE2000", reset);
}
