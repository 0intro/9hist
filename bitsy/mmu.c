#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

/* real protection bits */
enum
{
	/* level 1 descriptor bits */
	L1TypeMask=	(3<<0),
	L1Invalid=	(0<<0),
	L1PageTable=	(1<<0),
	L1Section=	(2<<0),
	L1Cached=	(1<<3),
	L1Buffered=	(1<<2),
	L1Domain0=	(0<<5),
	L1KernelRW=	(0x1<<10),
	L1UserRO=	(0x2<<10),
	L1UserRW=	(0x3<<10),
	L1SectBaseMask=	(0xFFF<<20),
	L1PTBaseMask=	(0x3FFFFF<<10),
	
	/* level 2 descriptor bits */
	L2TypeMask=	(3<<0),
	L2SmallPage=	(2<<0),
	L2LargePage=	(1<<0),
	L2Cached=	(1<<3),
	L2Buffered=	(1<<2),
	L2KernelRW=	(0x55<<4),
	L2UserRO=	(0xAA<<4),
	L2UserRW=	(0xFF<<4),
	L2PageBaseMask=	(0xFFFFF<<12),
};

/*
 *  table to map fault.c bits to physical bits
 */
static ulong phystrans[16] =
{
	[PTEVALID]				L2SmallPage|L2Cached|L2Buffered|L2UserRO,
	[PTEVALID|PTEWRITE]			L2SmallPage|L2Cached|L2Buffered|L2UserRW,
	[PTEVALID|PTEUNCACHED]			L2SmallPage|L2UserRO,
	[PTEVALID|PTEUNCACHED|PTEWRITE]		L2SmallPage|L2UserRW,

	[PTEKERNEL|PTEVALID]			L2SmallPage|L2Cached|L2Buffered|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEWRITE]		L2SmallPage|L2Cached|L2Buffered|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEUNCACHED]		L2SmallPage|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEUNCACHED|PTEWRITE]	L2SmallPage|L2KernelRW,
};

ulong *l1table;

/*
 *  We map all of memory, flash, and the zeros area with sections.
 *  Special use space is mapped on the fly with regmap.
 */
void
mmuinit(void)
{
	ulong a, e;

	/* set up the domain register to cause all domains to obey pte access bits */
	putdac(0x55555555);

	/* get a prototype level 1 page */
	l1table = xspanalloc(BY2PG, 16*1024, 0);
	memset(l1table, 0, BY2PG);

	/* direct map DRAM */
	e = conf.base1 + BY2PG*conf.npage2;
	for(a = PHYSDRAM0; a < e; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW |
				L1Cached | L1Buffered | (a&L1SectBaseMask);

	/* direct map zeros area */
	for(a = PHYSNULL0; a < PHYSNULL0 + 128 * OneMeg; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW |
				L1Cached | L1Buffered | (a&L1SectBaseMask);

	/* direct map flash */
	for(a = PHYFLASH0; a < PHYFLASH0 + 128 * OneMeg; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW |
				L1Cached | L1Buffered | (a&L1SectBaseMask);

	/* map the uart so that we can continue using iprint */
	uart3regs = mapspecial(UART3REGS, 64);
}

/*
 *  map special space, assume that the space isn't already mapped
 */
ulong*
mapspecial(ulong physaddr, int len)
{
	ulong *t;
	ulong virtaddr, i, base, end, off, entry, candidate;

	base = physaddr & ~(BY2PG-1);
	end = (physaddr+len-1) & ~(BY2PG-1);
	if(len > 128*1024)
		usemeg = 1;
	off = 0;
	candidate = 0;

	/* first see if we've mapped it somewhere, the first hole means we're done */
	for(virtaddr = REGZERO; virtaddr < REGTOP; virtaddr += OneMeg){
		if((l1table[virtaddr>>20] & L1TypeMask) != L1PageTable){
			/* create a page table and break */
			t = xspanalloc(BY2PG, 1024, 0);
			memzero(t, BY2PG, 0);
			l1table[virtaddr>>20] = L1PageTable | L1Domain0 |
						(((ulong)t) & L1PTBaseMask);
			break;
		}
		t = (ulong*)(l1table[virtaddr>>20] & L1PTBaseMask);
		for(i = 0; i < OneMeg; i += BY2PG){
			entry = t[(virtaddr+i)>>20];

			/* first hole means nothing left, add map */
			if((entry & L2TypeMask) != L2SmallPage)
				break;

			if(candidate == 0){
				/* look for start of range */
				if((entry & L2PageBaseMask) != base)
					continue;
				candidate = virtaddr+i;
			} else {
				/* look for contiunued range */
				if((entry & L2PageBaseMask) != base + off)
					candidate = 0;
					continue;
				}
			}

			/* if we're at the end of the range, area is already mapped */
			if((entry & L2PageBaseMask) == end)
				return candidate + (physaddr-base);
		}
		if(i < OneMeg){
			virtaddr += i;
			break;
		}
	}

	/* we get here if no entry was found mapping this physical range */
}

void
putmmu(ulong va, ulong pa, Page*)
{
	USED(va, pa);
}

void
mmurelease(Proc* proc)
{
	USED(proc);
}

void
mmuswitch(Proc* proc)
{
	USED(proc);
}
