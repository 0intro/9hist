/*
 * Trivial PCI configuration code.
 * Only deals with bus 0, amongst other glaring omissions.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

static Lock pcicfglock;

static int pcicfgmode = -1;

static void
pcicfginit(int)
{
	/*
	 * Try to determine which PCI configuration mode is implemented.
	 * Mode2 uses a byte at 0xCF8 and another at 0xCFA; Mode1 uses
	 * a DWORD at 0xCF8 and another at 0xCFC and will pass through
	 * any non-DWORD accesses as normal I/O cycles. There shouldn't be
	 * a device behind theses addresses so if Mode2 accesses fail try
	 * for Mode1 (which is preferred, Mode2 is deprecated).
	 */
	outb(PCIcse, 0);
	if(inb(PCIcse) == 0){
		pcicfgmode = 2;
		return;
	}

	outl(PCIaddr, 0);
	if(inl(PCIaddr) == 0){
		pcicfgmode = 1;
		return;
	}

	pcicfgmode = -1;
}

/*
 * Read a chunk of PCI configuration space.
 * Assumes arguments are within limits and regno and
 * nbytes are DWORD aligned.
 */
void
pcicfgr(int busno, int devno, int funcno, int regno, void* data, int nbytes)
{
	ulong addr, *p;
	int base, len;

	lock(&pcicfglock);
	if(pcicfgmode == -1)
		pcicfginit(busno);

	switch(pcicfgmode){

	case 1:
		addr = 0x80000000|((busno & 0xFF)<<16)|((devno & 0x1F)<<11)|((funcno & 0x03)<<8);
		p = data;
		for(len = nbytes/sizeof(ulong); len > 0; len--){
			outl(PCIaddr, addr|(regno & 0xFF));
			*p = inl(PCIdata);
			p++;
			regno += sizeof(ulong);
		}
	
		outl(PCIaddr, 0);
		break;

	case 2:
		if(devno >= 16)
			break;
		outb(PCIcse, 0x80|((funcno & 0x07)<<1));
		outb(PCIforward, busno);
	
		base = (0xC000|(devno<<8)) + regno;
		p = data;
		for(len = nbytes/sizeof(ulong); len > 0; len--){
			*p = inl(base);
			p++;
			base += sizeof(*p);
		}
	
		outb(PCIcse, 0);
		break;
	}

	unlock(&pcicfglock);
}

void
pcicfgw(int busno, int devno, int funcno, int regno, void* data, int nbytes)
{
	ulong addr, *p;
	int base, len;

	lock(&pcicfglock);
	if(pcicfgmode == -1)
		pcicfginit(busno);

	switch(pcicfgmode){

	case 1:
		addr = 0x80000000|((busno & 0xFF)<<16)|((devno & 0x1F)<<11)|((funcno & 0x03)<<8);
		p = data;
		for(len = nbytes/sizeof(*p); len > 0; len--){
			outl(PCIaddr, addr|(regno & 0xFF));
			outl(PCIdata, *p);
			p++;
			regno += sizeof(*p);
		}
	
		outl(PCIaddr, 0);
		break;

	case 2:
		if(devno >= 16)
			break;
		outb(PCIcse, 0x80|((funcno & 0x07)<<1));
		outb(PCIforward, busno);
	
		base = (0xC000|(devno<<8)) + regno;
		p = data;
		for(len = nbytes/sizeof(*p); len > 0; len--){
			outl(base, *p);
			p++;
			base += sizeof(*p);
		}
	
		outb(PCIcse, 0);
	}

	unlock(&pcicfglock);
}

/*
 * This is not in the spec, but at least the CMD640B requires it.
 */
void
pcicfgw8(int busno, int devno, int funcno, int regno, void* data, int nbytes)
{
	ulong addr;
	uchar *p;
	int base, len;

	lock(&pcicfglock);
	if(pcicfgmode == -1)
		pcicfginit(busno);

	switch(pcicfgmode){

	default:
		addr = 0x80000000|((busno & 0xFF)<<16)|((devno & 0x1F)<<11)|((funcno & 0x03)<<8);
		p = data;
		for(len = nbytes/sizeof(*p); len > 0; len--){
			outl(PCIaddr, addr|(regno & 0xFF));
			outb(PCIdata, *p);
			p++;
			regno += sizeof(*p);
		}
	
		outl(PCIaddr, 0);
		break;
		break;

	case 2:
		if(devno >= 16)
			break;
		outb(PCIcse, 0x80|((funcno & 0x07)<<1));
		outb(PCIforward, busno);
	
		base = (0xC000|(devno<<8)) + regno;
		p = data;
		for(len = nbytes/sizeof(*p); len > 0; len--){
			outb(base, *p);
			p++;
			base += sizeof(*p);
		}
	
		outb(PCIcse, 0);
	}

	unlock(&pcicfglock);
}

int
pcimatch(int busno, int devno, PCIcfg* pcicfg)
{
	ulong l;

	lock(&pcicfglock);
	if(pcicfgmode == -1)
		pcicfginit(busno);
	unlock(&pcicfglock);

	while(devno < MaxPCI){
		if(pcicfgmode == 2 && devno >= 16)
			break;
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
