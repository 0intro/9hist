/*
 * Size memory and create the kernel page-tables on the fly while doing so.
 * Called from main(), this code should only be run by the bootstrap processor.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

#define PDX(va)		((((ulong)(va))>>22) & 0x03FF)
#define PTX(va)		((((ulong)(va))>>12) & 0x03FF)

enum {
	MemUPA		= 0,		/* unbacked physical address */
	MemRAM		= 1,		/* physical memory */
	MemUMB		= 2,		/* upper memory block (<16MB) */
	NMemType	= 3,

	KB		= 1024,

	MemMinMB	= 4,		/* minimum physical memory (<=4MB) */
	MemMaxMB	= 512,		/* maximum physical memory to check */

	NMemBase	= 10,
};

typedef struct {
	int	size;
	ulong	addr;
} Map;

typedef struct {
	char*	name;
	Map*	map;
	Map*	mapend;

	Lock;
} RMap;

static Map mapupa[8];
static RMap rmapupa = {
	"unallocated unbacked physical memory",
	mapupa,
	&mapupa[7],
};

static Map xmapupa[8];
static RMap xrmapupa = {
	"unbacked physical memory",
	xmapupa,
	&xmapupa[7],
};

static Map mapram[8];
static RMap rmapram = {
	"physical memory",
	mapram,
	&mapram[7],
};

static Map mapumb[64];
static RMap rmapumb = {
	"upper memory block",
	mapumb,
	&mapumb[63],
};

static Map mapumbrw[8];
static RMap rmapumbrw = {
	"UMB device memory",
	mapumbrw,
	&mapumbrw[7],
};

#ifdef notdef
void
dumpmembank(void)
{
	Map *mp;
	ulong maxpa, maxpa1, maxpa2;

	maxpa = (nvramread(0x18)<<8)|nvramread(0x17);
	maxpa1 = (nvramread(0x31)<<8)|nvramread(0x30);
	maxpa2 = (nvramread(0x16)<<8)|nvramread(0x15);
	print("maxpa = %uX -> %uX, maxpa1 = %uX maxpa2 = %uX\n",
		maxpa, MB+maxpa*KB, maxpa1, maxpa2);

	for(mp = rmapram.map; mp->size; mp++)
		print("%8.8uX %8.8uX %8.8uX\n", mp->addr, mp->size, mp->addr+mp->size);
	for(mp = rmapumb.map; mp->size; mp++)
		print("%8.8uX %8.8uX %8.8uX\n", mp->addr, mp->size, mp->addr+mp->size);
	for(mp = rmapumbrw.map; mp->size; mp++)
		print("%8.8uX %8.8uX %8.8uX\n", mp->addr, mp->size, mp->addr+mp->size);
	for(mp = rmapupa.map; mp->size; mp++)
		print("%8.8uX %8.8uX %8.8uX\n", mp->addr, mp->size, mp->addr+mp->size);
}
#endif /* notdef */

void
mapfree(RMap* rmap, ulong addr, int size)
{
	Map *mp;
	ulong t;

	if(size <= 0)
		return;

	lock(rmap);
	for(mp = rmap->map; mp->addr <= addr && mp->size; mp++)
		;

	if(mp > rmap->map && (mp-1)->addr+(mp-1)->size == addr){
		(mp-1)->size += size;
		if(addr+size == mp->addr){
			(mp-1)->size += mp->size;
			while(mp->size){
				mp++;
				(mp-1)->addr = mp->addr;
				(mp-1)->size = mp->size;
			}
		}
	}
	else{
		if(addr+size == mp->addr && mp->size){
			mp->addr -= size;
			mp->size += size;
		}
		else do{
			if(mp >= rmap->mapend){
				print("mapfree: %s: losing 0x%uX, %d\n",
					rmap->name, addr, size);
				break;
			}
			t = mp->addr;
			mp->addr = addr;
			addr = t;
			t = mp->size;
			mp->size = size;
			mp++;
		}while(size = t);
	}
	unlock(rmap);
}

ulong
mapalloc(RMap* rmap, ulong addr, int size, int align)
{
	Map *mp;
	ulong maddr, oaddr;

	lock(rmap);
	for(mp = rmap->map; mp->size; mp++){
		maddr = mp->addr;

		if(addr){
			if(maddr > addr)
				continue;
			if(addr+size > maddr+mp->size)
				break;
			maddr = addr;
		}

		if(align > 0)
			maddr = ((maddr+align-1)/align)*align;
		if(mp->addr+mp->size-maddr < size)
			continue;

		oaddr = mp->addr;
		mp->addr = maddr+size;
		mp->size -= maddr-oaddr+size;
		if(mp->size == 0){
			do{
				mp++;
				(mp-1)->addr = mp->addr;
			}while((mp-1)->size = mp->size);
		}

		unlock(rmap);
		if(oaddr != maddr)
			mapfree(rmap, oaddr, maddr-oaddr);

		return maddr;
	}
	unlock(rmap);

	return 0;
}

static void
umbscan(void)
{
	uchar *p;

	/*
	 * Scan the Upper Memory Blocks (0xA0000->0xF0000) for pieces
	 * which aren't used; they can be used later for devices which
	 * want to allocate some virtual address space.
	 * Check for two things:
	 * 1) device BIOS ROM. This should start with a two-byte header
	 *    of 0x55 0xAA, followed by a byte giving the size of the ROM
	 *    in 512-byte chunks. These ROM's must start on a 2KB boundary.
	 * 2) device memory. This is read-write.
	 * There are some assumptions: there's VGA memory at 0xA0000 and
	 * the VGA BIOS ROM is at 0xC0000. Also, if there's no ROM signature
	 * at 0xE0000 then the whole 64KB up to 0xF0000 is theoretically up
	 * for grabs; check anyway.
	 */
	p = KADDR(0xC8000);
	while(p < (uchar*)KADDR(0xE0000)){
		p[0] = 0xCC;
		p[2*KB-1] = 0xCC;
		if(p[0] != 0xCC || p[2*KB-1] != 0xCC){
			p[0] = 0x55;
			p[1] = 0xAA;
			p[2] = 4;
			if(p[0] == 0x55 && p[1] == 0xAA){
				p += p[2]*512;
				continue;
			}
			mapfree(&rmapumb, PADDR(p), 2*KB);
		}
		else
			mapfree(&rmapumbrw, PADDR(p), 2*KB);
		p += 2*KB;
	}

	p = KADDR(0xE0000);
	if(p[0] != 0x55 || p[1] != 0xAA){
		p[0] = 0xCC;
		p[64*KB-1] = 0xCC;
		if(p[0] != 0xCC && p[64*KB-1] != 0xCC)
			mapfree(&rmapumb, PADDR(p), 64*KB);
	}
}

static void
ramscan(ulong maxmem)
{
	ulong *k0, kzero, map, maxpa, pa, *pte, *table, *va, x;
	int nvalid[NMemType];
	uchar *bda;

	/*
	 * The bootstrap code has has created a prototype page
	 * table which maps the first MemMinMB of physical memory to KZERO.
	 * The page directory is at m->pdb and the first page of
	 * free memory is after the per-processor MMU information.
	 */
	/*
	 * Initialise the memory bank information for conventional memory
	 * (i.e. less than 640KB). The base is the first location after the
	 * bootstrap processor MMU information and the limit is obtained from
	 * the BIOS data area.
	 */
	x = PADDR(CPU0MACH+BY2PG);
	bda = (uchar*)(KZERO|0x400);
	mapfree(&rmapram, x, ((bda[0x14]<<8)|bda[0x13])*KB-x);

	/*
	 * Check if the extended memory size can be obtained from the CMOS.
	 * If it's 0 then it's either not known or >= 64MB. Always check
	 * at least 24MB in case there's a memory gap (up to 8MB) below 16MB;
	 * in this case the memory from the gap is remapped to the top of
	 * memory.
	 * The value in CMOS is supposed to be the number of KB above 1MB.
	 */
	if(maxmem == 0){
		x = (nvramread(0x18)<<8)|nvramread(0x17);
		if(x == 0 || x >= (63*KB))
			maxpa = MemMaxMB*MB;
		else
			maxpa = MB+x*KB;
		if(maxpa < 24*MB)
			maxpa = 24*MB;
		maxmem = MemMaxMB*MB;
	}
	else
		maxpa = maxmem;

	/*
	 * March up memory from the end of the loaded kernel to maxpa
	 * a page at a time, mapping the page and checking the page can
	 * be written and read correctly. The page tables are created here
	 * on the fly, allocating from low memory as necessary.
	 * Nvalid must be initialised to include some UMB's to prevent the
	 * first 4MB being mapped with a 4MB page extension, it's already
	 * partly mapped.
	 */
	k0 = (ulong*)KZERO;
	kzero = *k0;
	map = 0;
	x = 0x12345678;
	memset(nvalid, 0, sizeof(nvalid));
	nvalid[MemUMB] = (0x100000-0xC8000)/BY2PG;
	nvalid[MemRAM] = PADDR(PGROUND((ulong)end))/BY2PG - nvalid[MemUMB];
	for(pa = PADDR(PGROUND((ulong)end)); pa < maxpa; pa += BY2PG){
		/*
		 * Map the page. Use mapalloc(&rmapram, ...) to make
		 * the page table if necessary, it will be returned to the
		 * pool later if it isn't needed.
		 */
		va = KADDR(pa);
		table = &((ulong*)m->pdb)[PDX(va)];
		if(*table == 0){
			if(map == 0 && (map = mapalloc(&rmapram, 0, BY2PG, BY2PG)) == 0)
				break;
			memset(KADDR(map), 0, BY2PG);
			*table = map|PTEWRITE|PTEVALID;
			memset(nvalid, 0, sizeof(nvalid));
		}
		table = KADDR(PPN(*table));
		pte = &table[PTX(va)];

		*pte = pa|PTEWRITE|PTEUNCACHED|PTEVALID;
		mmuflushtlb(PADDR(m->pdb));

		/*
		 * Write a pattern to the page and write a different
		 * pattern to a possible mirror at KZER0. If the data
		 * reads back correctly the page is some type of RAM (possibly
		 * a linearly-mapped VGA framebuffer, for instance...) and
		 * can be cleared and added to the memory pool. If not, the
		 * page is marked invalid and added to the UMB or NOT pool
		 * depending on whether it is <16MB or not.
		 */
		*va = x;
		*k0 = ~x;
		if(*va == x){
			*pte &= ~PTEUNCACHED;
			nvalid[MemRAM]++;
			mapfree(&rmapram, pa, BY2PG);
			memset(va, 0, BY2PG);
		}
		else{
			if(pa < 16*MB){
				nvalid[MemUMB]++;
				mapfree(&rmapumb, pa, BY2PG);
			}
			else{
				*pte = 0;
				nvalid[MemUPA]++;
				mapfree(&rmapupa, pa, BY2PG);
			}
		}

		/*
		 * Done with this 4MB chunk, review the options:
		 * 1) not physical memory and >=16MB - invalidate the PDB entry;
		 * 2) physical memory - use the 4MB page extension if possible;
		 * 3) not physical memory and <16MB - use the 4MB page extension
		 *    if possible;
		 * 4) mixed or no 4MB page extension - commit the already
		 *    initialised space for the page table.
		 */
		if(((pa+BY2PG) % (4*MB)) == 0){
			table = &((ulong*)m->pdb)[PDX(va)];
			if(nvalid[MemUPA] == 1024)
				*table = 0;
			else if(nvalid[MemRAM] == 1024 && (m->cpuiddx & 0x08))
				*table = (pa & ~(4*MB-1))|PTESIZE|PTEWRITE|PTEVALID;
			else if(nvalid[MemUMB] == 1024 && (m->cpuiddx & 0x08))
				*table = (pa & ~(4*MB-1))|PTESIZE|PTEWRITE|PTEUNCACHED|PTEVALID;
			else
				map = 0;
		}

		mmuflushtlb(PADDR(m->pdb));
		x += 0x3141526;
	}
	if(map)
		mapfree(&rmapram, map, BY2PG);
	if(pa < maxmem)
		mapfree(&rmapupa, pa, maxmem-pa);
	*k0 = kzero;
}

static void
upainit(void)
{
	ulong addr, pa, pae, *table, *va, x;

	/*
	 * Allocate an 8MB chunk aligned to 16MB. Later can
	 * make the region selectable via conf if necessary.
	 */
	if((addr = mapalloc(&rmapupa, 0, 8*MB, 16*MB)) == 0)
		return;

	pa = addr;
	pae = pa+(16*MB);
	while(pa < pae){
		va = KADDR(pa);
		table = &((ulong*)m->pdb)[PDX(va)];
		if((pa % (4*MB)) == 0 && (m->cpuiddx & 0x08)){
			*table = pa|PTESIZE|PTEWRITE|PTEUNCACHED|PTEVALID;
			pa += (4*MB);
		}
		else{
			if(*table == 0){
				if((x = mapalloc(&rmapram, 0, BY2PG, BY2PG)) == 0)
					break;
				memset(KADDR(x), 0, BY2PG);
				*table = x|PTEWRITE|PTEVALID;
			}
			table = (ulong*)(KZERO|PPN(*table));
			table[PTX(va)] = pa|PTEWRITE|PTEUNCACHED|PTEVALID;
			pa += BY2PG;
		}
	}

	mapfree(&xrmapupa, addr, pa-addr);
}


void
meminit(ulong maxmem)
{
	Map *mp, *xmp;
	ulong pa, *pte;

	/*
	 * Set special attributes for memory between 640KB and 1MB:
	 *   VGA memory is writethrough;
	 *   BIOS ROM's/UMB's are uncached;
	 * then scan for useful memory.
	 */
	for(pa = 0xA0000; pa < 0xC0000; pa += BY2PG){
		pte = mmuwalk(m->pdb, (ulong)KADDR(pa), 0);
		*pte |= PTEWT;
	}
	for(pa = 0xC0000; pa < 0x100000; pa += BY2PG){
		pte = mmuwalk(m->pdb, (ulong)KADDR(pa), 0);
		*pte |= PTEUNCACHED;
	}
	mmuflushtlb(PADDR(m->pdb));

	umbscan();
	ramscan(maxmem);
	upainit();

	/*
	 * Set the conf entries describing two banks of allocatable memory.
	 * Grab the first and largest entries in rmapram as left by ramscan().
	 *
	 * It would be nice to have more than 2 memory banks describable in conf.
	 */
	mp = rmapram.map;
	conf.base0 = mp->addr;
	conf.npage0 = mp->size/BY2PG;
	mp++;
	for(xmp = 0; mp->size; mp++){
		if(xmp == 0 || mp->size > xmp->size)
			xmp = mp;
	}

	if(xmp){		
		conf.base1 = xmp->addr;
		conf.npage1 = xmp->size/BY2PG;
	}
}

ulong
umbmalloc(ulong addr, int size, int align)
{
	ulong a;

	if(a = mapalloc(&rmapumb, addr, size, align))
		return KZERO|a;

	return 0;
}

void
umbfree(ulong addr, int size)
{
	mapfree(&rmapumb, addr & ~KZERO, size);
}

ulong
umbrwmalloc(ulong addr, int size, int align)
{
	ulong a;
	uchar *p;

	if(a = mapalloc(&rmapumbrw, addr, size, align))
		return KZERO|a;

	/*
	 * Perhaps the memory wasn't visible before
	 * the interface is initialised, so try again.
	 */
	if((a = umbmalloc(addr, size, align)) == 0)
		return 0;
	p = (uchar*)a;
	p[0] = 0xCC;
	p[size-1] = 0xCC;
	if(p[0] == 0xCC && p[size-1] == 0xCC)
		return a;
	umbfree(a, size);

	return 0;
}

void
umbrwfree(ulong addr, int size)
{
	mapfree(&rmapumbrw, addr & ~KZERO, size);
}

ulong
upamalloc(ulong addr, int size, int align)
{
	ulong a;

	if(a = mapalloc(&xrmapupa, addr, size, align))
		return KZERO|a;

	return 0;
}

void
upafree(ulong addr, int size)
{
	mapfree(&xrmapupa, addr & ~KZERO, size);
}
