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
	Small_Page=	(2<<0),
	Large_Page=	(1<<0),
	Cached=		(1<<3),
	Buffered=	(1<<2),
	UserRO=		(0xAA<<4),
	UserRW=		(0xFF<<4),
	KernelRW=	(0x55<<4),
};


/*
 *  table to map fault.c bits to physical bits
 */
static ulong phystrans[8] =
{
	[PTEVALID]			Small_Page|Cached|Buffered|UserRO,
	[PTEVALID|PTEWRITE]		Small_Page|Cached|Buffered|UserRW,
	[PTEVALID|UNCACHED]		Small_Page|UserRO,
	[PTEVALID|UNCACHED|PTEWRITE]	Small_Page|UserRW,
};

ulong *l1page;

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
	l1page = xspanalloc(BY2PG, 16*1024, 0);
	memset(l1page, 0, BY2PG);

	/* map DRAM */
	e = PHYSDRAM0 + BY2PG*con
	for(
	/* map zeros */
	/* map flash */
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
