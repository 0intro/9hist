#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

static Lock pcicfglock;

/*
 * Read a chunk of PCI configuration space.
 * Assumes arguments are within limits and regno and
 * nbytes are DWORD aligned.
 */
void
pcicfgr(int busno, int devno, int funcno, int regno, void* data, int nbytes)
{
	ulong* p;
	int base, len;

	lock(&pcicfglock);
	outb(PCIcse, 0x80|((funcno & 0x07)<<1));
	outb(PCIforward, busno);

	base = (0xC000|(devno<<8)) + regno;
	p = data;
	for(len = nbytes/sizeof(ulong); len > 0; len--){
		*p = inl(base);
		p++;
		base += sizeof(*p);
	}

	outb(PCIcse, 0x00);
	unlock(&pcicfglock);
}

void
pcicfgw(int busno, int devno, int funcno, int regno, void* data, int nbytes)
{
	ulong* p;
	int base, len;

	lock(&pcicfglock);
	outb(PCIcse, 0x80|((funcno & 0x07)<<1));
	outb(PCIforward, busno);

	base = (0xC000|(devno<<8)) + regno;
	p = data;
	for(len = nbytes/sizeof(ulong); len > 0; len--){
		outl(base, *p);
		p++;
		base += sizeof(*p);
	}

	outb(PCIcse, 0x00);
	unlock(&pcicfglock);
}

int
pcimatch(int busno, int devno, PCIcfg* pcicfg)
{
	ulong l;

	while(devno < MaxPCI){
		l = 0;
		pcicfgr(busno, devno, 0, 0, &l, sizeof(ulong));
		devno++;
		if((l & 0xFFFF) != pcicfg->vid)
			continue;
		if(pcicfg->did && ((l>>16) & 0xFFFF) != pcicfg->did)
			continue;
		pcicfgr(busno, devno-1, 0, 0, pcicfg, sizeof(PCIcfg));
		return devno;
	}
	return -1;
}
