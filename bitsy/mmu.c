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
	ulong *t;

	/* get a prototype level 1 page */
	l1table = xspanalloc(BY2PG, 16*1024, 0);
	memset(l1table, 0, BY2PG);

	/* direct map DRAM */
	e = conf.base1 + BY2PG*conf.npage1;
	for(a = PHYSDRAM0; a < e; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW | (a&L1SectBaseMask) |
				L1Cached | L1Buffered;

	/* direct map zeros area */
	for(a = PHYSNULL0; a < PHYSNULL0 + 128 * OneMeg; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW | (a&L1SectBaseMask);

	/* direct map flash */
	for(a = PHYSFLASH0; a < PHYSFLASH0 + 128 * OneMeg; a += OneMeg)
		l1table[a>>20] = L1Section | L1KernelRW | (a&L1SectBaseMask) |
				L1Cached | L1Buffered;

	/* map first page of DRAM also into 0xFFFF0000 for the interrupt vectors */
	t = xspanalloc(BY2PG, 16*1024, 0);
	memset(t, 0, BY2PG);
	l1table[0xFFFF0000>>20] = L1PageTable | L1Domain0 | (((ulong)t) & L1PTBaseMask);
	t[0xF0000>>PGSHIFT] = L2SmallPage | L2KernelRW | PHYSDRAM0;

	/* set up the domain register to cause all domains to obey pte access bits */
	iprint("setting up domain access\n");
	putdac(0xFFFFFFFF);

	/* point to map */
	iprint("setting tlb map %lux\n", (ulong)l1table);
	putttb((ulong)l1table);

	/* map the uart so that we can continue using iprint */
	uart3regs = mapspecial(UART3REGS, 64);

	/* enable mmu, and make 0xFFFF0000 the virtual address of the exception vecs */
	mmuenable();

	iprint("uart3regs now at %lux\n", uart3regs);
}

/*
 *  map special space uncached, assume that the space isn't already mapped
 */
void*
mapspecial(ulong pa, int len)
{
	ulong *t;
	ulong va, i, base, end, off, entry;
	int livelarge;
	ulong* rv;

	rv = nil;
	livelarge = len >= 128*1024;
	if(livelarge){
		base = pa & ~(OneMeg-1);
		end = (pa+len-1) & ~(OneMeg-1);
	} else {
		base = pa & ~(BY2PG-1);
		end = (pa+len-1) & ~(BY2PG-1);
	}
	off = pa - base;

	for(va = REGZERO; va < REGTOP && base <= end; va += OneMeg){
		switch(l1table[va>>20] & L1TypeMask){
		default:
			/* found unused entry on level 1 table */
			if(livelarge){
				if(rv == nil)
					rv = (ulong*)(va+off);
				l1table[va>>20] = L1Section | L1KernelRW |
							(base & L1SectBaseMask);
				base += OneMeg;
				continue;
			} else {

				/* create an L2 page table and keep going */
				t = xspanalloc(BY2PG, 1024, 0);
				memset(t, 0, BY2PG);
				l1table[va>>20] = L1PageTable | L1Domain0 |
							(((ulong)t) & L1PTBaseMask);
			}
			break;
		case L1Section:
			continue;
		case L1PageTable:
			if(livelarge)
				continue;
			break;
		}

		/* here if we're using page maps instead of sections */
		t = (ulong*)(l1table[va>>20] & L1PTBaseMask);
		for(i = 0; i < OneMeg; i += BY2PG){
			entry = t[i>>PGSHIFT];

			/* found unused entry on level 2 table */
			if((entry & L2TypeMask) != L2SmallPage){
				if(rv == nil)
					rv = (ulong*)(va+i+off);
				t[i>>PGSHIFT] = L2SmallPage | L2KernelRW | 
						(base & L2PageBaseMask);
				base += BY2PG;
				continue;
			}
		}
	}

	/* didn't fit */
	if(base <= end)
		return nil;

	return rv;
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
