/*
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

static int
reset(Ether* ether)
{
	int slot;

	if(ether->irq == 0)
		ether->irq = 10;
	if(ether->port == 0)
		ether->port = 0x240;

	if((slot = pcmspecial(ether->type, ether)) < 0)
		return -1;

	print("WaveLAN: slot %d, port 0x%uX irq %d\n", slot, ether->port, ether->irq);

	return -1;
}

void
etherwavelanlink(void)
{
	addethercard("WaveLAN", reset);
}
