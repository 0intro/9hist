/*
 * PCI support code.
 * To do:
 *	initialise bridge mappings if the PCI BIOS didn't.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

enum {					/* configuration mechanism #1 */
	PciADDR		= 0xCF8,	/* CONFIG_ADDRESS */
	PciDATA		= 0xCFC,	/* CONFIG_DATA */

					/* configuration mechanism #2 */
	PciCSE		= 0xCF8,	/* configuration space enable */
	PciFORWARD	= 0xCFA,	/* which bus */

	MaxFNO		= 7,
	MaxUBN		= 255,
};

static Lock pcicfglock;
static Lock pcicfginitlock;
static int pcicfgmode = -1;
static int pcimaxdno;
static Pcidev* pciroot;
static Pcidev* pcilist;
static Pcidev* pcitail;

static int pcicfgrw8(int, int, int, int);
static int pcicfgrw16(int, int, int, int);
static int pcicfgrw32(int, int, int, int);

static int
pciscan(int bno, Pcidev** list)
{
	ulong v;
	Pcidev *p, *head, *tail;
	int dno, fno, i, l, maxfno, maxubn, rno, sbn, tbdf, ubn;

	maxubn = bno;
	head = nil;
	tail = nil;
	for(dno = 0; dno <= pcimaxdno; dno++){
		maxfno = 0;
		for(fno = 0; fno <= maxfno; fno++){
			/*
			 * For this possible device, form the bus+device+function
			 * triplet needed to address it and try to read the vendor
			 * and device ID. If successful, allocate a device struct
			 * and start to fill it in with some useful information from
			 * the device's configuration space.
			 */
			tbdf = MKBUS(BusPCI, bno, dno, fno);
			l = pcicfgrw32(tbdf, PciVID, 0, 1);
			if(l == 0xFFFFFFFF || l == 0)
				continue;
			p = malloc(sizeof(*p));
			p->tbdf = tbdf;
			p->vid = l;
			p->did = l>>16;

			if(pcilist != nil)
				pcitail->list = p;
			else
				pcilist = p;
			pcitail = p;

			p->intl = pcicfgrw8(tbdf, PciINTL, 0, 1);
			p->ccru = pcicfgrw16(tbdf, PciCCRu, 0, 1);

			rno = PciBAR0 - 4;
			for(i = 0; i < nelem(p->mem); i++) {
				rno += 4;
				p->mem[i].bar = pcicfgrw32(tbdf, rno, 0, 1);
				if(i > 0 && p->ccru == ((0x06<<8)|0x04))
					continue;
				pcicfgrw32(tbdf, rno, -1, 0);
				v = pcicfgrw32(tbdf, rno, 0, 1);
				pcicfgrw32(tbdf, rno, p->mem[i].bar, 0);
				p->mem[i].size = -(v & ~0xF);
			}

			if(head != nil)
				tail->link = p;
			else
				head = p;
			tail = p;

			/*
			 * If the device is a multi-function device adjust the
			 * loop count so all possible functions are checked.
			 */
			l = pcicfgrw8(tbdf, PciHDT, 0, 1);
			if(l & 0x80)
				maxfno = MaxFNO;
		}
	}

	*list = head;
	for(p = head; p != nil; p = p->link){
		/*
		 * Find bridges and recursively descend the tree.
		 * Special case the Intel 82454GX Host-to-PCI bridge,
		 * there can be two of them.
		 * Otherwise, only descend PCI-to-PCI bridges.
		 */
		if(p->ccru == ((0x06<<8)|0) && p->vid == 0x8086 && p->did == 0x84C4){
			tbdf = p->tbdf;
			if((sbn = pcicfgrw8(tbdf, 0x4A, 0, 1)) == 0)
				continue;
			ubn = pcicfgrw8(tbdf, 0x4B, 0, 1);
			maxubn = ubn;
			pciscan(sbn, &p->bridge);
			continue;
		}
		if(p->ccru != ((0x06<<8)|0x04))
			continue;

		/*
		 * If the secondary or subordinate bus number is not initialised
		 * try to do what the PCI BIOS should have done and fill in the
		 * numbers as the tree is descended. On the way down the subordinate
		 * bus number is set to the maximum as it's not known how many
		 * buses are behind this one; the final value is set on the way
		 * back up.
		 */
		tbdf = p->tbdf;
		sbn = pcicfgrw8(tbdf, PciSBN, 0, 1);
		ubn = pcicfgrw8(tbdf, PciUBN, 0, 1);
		if(sbn == 0 || ubn == 0){
			sbn = maxubn+1;
			/*
			 * Make sure memory, I/O and master enables are off,
			 * set the primary, secondary and subordinate bus numbers
			 * and clear the secondary status before attempting to
			 * scan the secondary bus.
			 *
			 * Initialisation of the bridge should be done here.
			 */
			pcicfgrw32(tbdf, PciPCR, 0xFFFF0000, 0);
			l = (MaxUBN<<16)|(sbn<<8)|bno;
			pcicfgrw32(tbdf, PciPBN, l, 0);
			pcicfgrw16(tbdf, PciSPSR, 0xFFFF, 0);
			maxubn = pciscan(sbn, &p->bridge);
			l = (maxubn<<16)|(sbn<<8)|bno;
			pcicfgrw32(tbdf, PciPBN, l, 0);
		}
		else{
			maxubn = ubn;
			pciscan(sbn, &p->bridge);
		}
	}

	return maxubn;
}

static void
pcicfginit(void)
{
	lock(&pcicfginitlock);
	if(pcicfgmode == -1){
		/*
		 * Try to determine which PCI configuration mode is implemented.
		 * Mode2 uses a byte at 0xCF8 and another at 0xCFA; Mode1 uses
		 * a DWORD at 0xCF8 and another at 0xCFC and will pass through
		 * any non-DWORD accesses as normal I/O cycles. There shouldn't be
		 * a device behind theses addresses so if Mode2 accesses fail try
		 * for Mode1 (which is preferred, Mode2 is deprecated).
		 */
		outb(PciCSE, 0);
		if(inb(PciCSE) == 0){
			pcicfgmode = 2;
			pcimaxdno = 15;
		}
		else{
			outl(PciADDR, 0);
			if(inl(PciADDR) == 0){
				pcicfgmode = 1;
				pcimaxdno = 31;
			}
		}
	
		if(pcicfgmode > 0)
			pciscan(0, &pciroot);
	}
	unlock(&pcicfginitlock);
}

static int
pcicfgrw8(int tbdf, int rno, int data, int read)
{
	int o, type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		o = rno & 0x03;
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = inb(PciDATA+o);
		else
			outb(PciDATA+o, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = inb((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outb((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr8(Pcidev* pcidev, int rno)
{
	return pcicfgrw8(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw8(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw8(pcidev->tbdf, rno, data, 0);
}

static int
pcicfgrw16(int tbdf, int rno, int data, int read)
{
	int o, type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		o = rno & 0x02;
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = ins(PciDATA+o);
		else
			outs(PciDATA+o, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = ins((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outs((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr16(Pcidev* pcidev, int rno)
{
	return pcicfgrw16(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw16(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw16(pcidev->tbdf, rno, data, 0);
}

static int
pcicfgrw32(int tbdf, int rno, int data, int read)
{
	int type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = inl(PciDATA);
		else
			outl(PciDATA, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = inl((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outl((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr32(Pcidev* pcidev, int rno)
{
	return pcicfgrw32(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw32(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw32(pcidev->tbdf, rno, data, 0);
}

ulong
pcibarsize(Pcidev* p, int rno)
{
	ulong v, size;

	v = pcicfgrw32(p->tbdf, rno, 0, 1);
	pcicfgrw32(p->tbdf, rno, 0xFFFFFFF0, 0);
	size = pcicfgrw32(p->tbdf, rno, 0, 1);
	pcicfgrw32(p->tbdf, rno, v, 0);

	return -(size & ~0x0F);
}

Pcidev*
pcimatch(Pcidev* prev, int vid, int did)
{
	if(pcicfgmode == -1)
		pcicfginit();

	if(prev == nil)
		prev = pcilist;
	else
		prev = prev->list;

	while(prev != nil) {
		if(prev->vid == vid && (did == 0 || prev->did == did))
			break;
		prev = prev->list;
	}
	return prev;
}

void
pcireset(void)
{
	Pcidev *p;
	int pcr;

	if(pcicfgmode == -1)
		pcicfginit();

	for(p = pcilist; p != nil; p = p->list){
		pcr = pcicfgr16(p, PciPSR);
		pcicfgw16(p, PciPSR, pcr & ~0x04);
	}
}

void
pcihinv(Pcidev* p)
{
	int i;
	Pcidev *t;

	if(p == nil) {
		p = pciroot;
		print("bus dev type vid  did  memory\n");
	}
	for(t = p; t != nil; t = t->link) {
		print("%d  %2d/%d %.4ux %.4ux %.4ux ",
			BUSBNO(t->tbdf), BUSDNO(t->tbdf), BUSFNO(t->tbdf),
			t->ccru, t->vid, t->did);

		for(i = 0; i < nelem(p->mem); i++) {
			if(t->mem[i].size == 0)
				continue;
			print("%d:%.8lux %d ", i,
				t->mem[i].bar, t->mem[i].size);
		}
		print("\n");
	}
	while(p != nil) {
		if(p->bridge != nil)
			pcihinv(p->bridge);
		p = p->link;
	}
}

/*
 * Hack for now to get SYMBIOS controller on-line.
 */
void*
pcimemmap(int tbdf, int rno, ulong *paddr)
{
	long size;
	ulong p, v;

	if(pcicfgmode == -1)
		pcicfginit();

	v = pcicfgrw32(tbdf, rno, 0, 1);
	if(v & 1){
		print("pcimemmap: not a memory base register\n");
		return 0;
	}
	if(v & 6){
		print("pcimemmap: only 32 bit relocs supported\n");
		return 0;
	}
	v = 0xFFFFFFFF;
	pcicfgrw32(tbdf, rno, v, 0);
	v = pcicfgrw32(tbdf, rno, 0, 1);
	/*
	 * Clear out bottom bits and negate to find size.
	 * If none can be found could try for UPA memory here.
	 */
	size = -(v & ~0x0F);
	v = umbmalloc(0, size, size);
	p = PADDR(v);
	if(paddr)
		*paddr = p;
	pcicfgrw32(tbdf, rno, p, 0);
	return (void*)v;
}
