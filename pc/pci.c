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
pcicfgr(uchar busno, uchar devno, uchar funcno, uchar regno, void* data, int nbytes)
{
	ulong base, *p;
	int len;

	lock(&pcicfglock);
	outb(PCIcse, 0x80|((funcno & 0x07)<<1));
	outb(PCIforward, busno);

	base = (0xC000|(devno<<8)) + regno;
	p = data;
	for(len = nbytes/sizeof(ulong); len > 0; len--){
		*p = inl(base);
		p++;
		base += sizeof(ulong);
	}

	outb(PCIcse, 0x00);
	unlock(&pcicfglock);
}

int
pcimatch(uchar busno, uchar devno, PCIcfg* pcicfg)
{
	ulong l;

	l = 0;
	pcicfgr(busno, devno, 0, 0, &l, sizeof(ulong));
	if((l & 0xFFFF) != pcicfg->vid)
		return 0;
	if(pcicfg->did && ((l>>16) & 0xFFFF) != pcicfg->did)
		return 0;
	pcicfgr(busno, devno, 0, 0, pcicfg, sizeof(PCIcfg));

	return 1;
}
