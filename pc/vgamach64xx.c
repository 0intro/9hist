#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

/*
 * ATI Mach64(CT|ET|G*|VT|VU).
 */
static ushort mach64xxdid[] = {
	('C'<<8)|'T',
	('E'<<8)|'T',
	('G'<<8)|'B',
	('G'<<8)|'D',
	('G'<<8)|'I',
	('G'<<8)|'P',
	('G'<<8)|'Q',
	('G'<<8)|'T',
	('G'<<8)|'U',
	('V'<<8)|'T',
	('V'<<8)|'U',
	0,
};

static Pcidev*
mach64xxpci(void)
{
	Pcidev *p;
	ushort *did;

	if((p = pcimatch(nil, 0x1002, 0)) == nil)
		return nil;
	for(did = mach64xxdid; *did; did++){
		if(*did == p->did)
			return p;
	}

	return nil;
}

static void
mach64xxenable(VGAscr* scr)
{
	Pcidev *p;

	/*
	 * Only once, can't be disabled for now.
	 */
	if(scr->io)
		return;
	if(p = mach64xxpci()){
		/*
		 * The CT doesn't always have the I/O base address
		 * in the PCI base registers. There is a way to find
		 * it via the vendor-specific PCI config space but
		 * this will do for now.
		 */
		scr->io = p->mem[1].bar & ~0x03;
		if(scr->io == 0 && p->did == ('C'<<8)|'T')
			scr->io = 0x2EC;
	}
}

static ulong
mach64xxlinear(VGAscr* scr, int* size, int* align)
{
	ulong aperture, oaperture;
	int oapsize, wasupamem;
	Pcidev *p;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;
	if(wasupamem)
		upafree(oaperture, oapsize);
	scr->isupamem = 0;

	if(p = mach64xxpci()){
		aperture = p->mem[0].bar & ~0x0F;
		*size = p->mem[0].size;
	}
	else
		aperture = 0;

	aperture = upamalloc(aperture, *size, *align);
	if(aperture == 0){
		if(wasupamem && upamalloc(oaperture, oapsize, 0))
			scr->isupamem = 1;
	}
	else
		scr->isupamem = 1;

	return aperture;
}

enum {
	CurClr0		= 0x0B,		/* I/O Select */
	CurClr1		= 0x0C,
	CurOffset	= 0x0D,
	CurHVposn	= 0x0E,
	CurHVoff	= 0x0F,

	GenTestCntl	= 0x19,
};

static uchar mmoffset[] = {
	[CurClr0]	0x18,
	[CurClr1]	0x19,
	[CurOffset]	0x1A,
	[CurHVposn]	0x1B,
	[CurHVoff]	0x1C,

	[GenTestCntl]	0x34,
};

static ulong
ior32(VGAscr* scr, int r)
{
	if(scr->io == 0x2EC || scr->io == 0x1C8)
		return inl((r<<10)+scr->io);
	return inl((mmoffset[r]<<2)+scr->io);
}

static void
iow32(VGAscr* scr, int r, ulong l)
{
	if(scr->io == 0x2EC || scr->io == 0x1C8)
		outl(((r)<<10)+scr->io, l);
	else
		outl((mmoffset[r]<<2)+scr->io, l);
}

static void
mach64xxcurdisable(VGAscr* scr)
{
	ulong r;

	r = ior32(scr, GenTestCntl);
	iow32(scr, GenTestCntl, r & ~0x80);
}

static void
mach64xxcurload(VGAscr* scr, Cursor* curs)
{
	uchar *p;
	int i, y;
	ulong c, s, m, r;

	/*
	 * Disable the cursor.
	 */
	r = ior32(scr, GenTestCntl);
	iow32(scr, GenTestCntl, r & ~0x80);

	p = KADDR(scr->aperture);
	p += scr->storage;

	/*
	 * Initialise the 64x64 cursor RAM array.
	 * The cursor mode gives the following truth table:
	 *	p1 p0	colour
	 *	 0  0	Cursor Colour 0
	 *	 0  1	Cursor Colour 1
	 *	 1  0	Transparent
	 *	 1  1	Complement
	 * Put the cursor into the top-right of the 64x64 array.
	 */
	for(y = 0; y < 16; y++){
		for(i = 0; i < (64-16)/8; i++){
			*p++ = 0xAA;
			*p++ = 0xAA;
		}

		c = (curs->clr[2*y]<<8)|curs->clr[y*2 + 1];
		s = (curs->set[2*y]<<8)|curs->set[y*2 + 1];

		m = 0x00000000;
		for(i = 0; i < 16; i++){
			if(s & (1<<(15-i)))
				m |= 0x01<<(2*i);
			else if(c & (1<<(15-i)))
				;
			else
				m |= 0x02<<(2*i);
		}
		*p++ = m;
		*p++ = m>>8;
		*p++ = m>>16;
		*p++ = m>>24;
	}
	memset(p, 0xAA, (64-16)*16);

	/*
	 * Set the cursor hotpoint and enable the cursor.
	 */
	scr->offset = curs->offset;
	iow32(scr, GenTestCntl, 0x80|r);
}

static int
mach64xxcurmove(VGAscr* scr, Point p)
{
	int x, xo, y, yo;

	/*
	 * Mustn't position the cursor offscreen even partially,
	 * or it disappears. Therefore, if x or y is -ve, adjust the
	 * cursor presets instead. If y is negative also have to
	 * adjust the starting offset.
	 */
	if((x = p.x+scr->offset.x) < 0){
		xo = x;
		x = 0;
	}
	else
		xo = 0;
	if((y = p.y+scr->offset.y) < 0){
		yo = y;
		y = 0;
	}
	else
		yo = 0;

	iow32(scr, CurHVoff, ((64-16-yo)<<16)|(64-16-xo));
	iow32(scr, CurOffset, scr->storage/8 + (-yo*2));
	iow32(scr, CurHVposn, (y<<16)|x);

	return 0;
}

static void
mach64xxcurenable(VGAscr* scr)
{
	ulong r, storage;

	mach64xxenable(scr);
	if(scr->io == 0)
		return;

	r = ior32(scr, GenTestCntl);
	iow32(scr, GenTestCntl, r & ~0x80);

	iow32(scr, CurClr0, (Pwhite<<24)|(Pwhite<<16)|(Pwhite<<8)|Pwhite);
	iow32(scr, CurClr1, (Pblack<<24)|(Pblack<<16)|(Pblack<<8)|Pblack);

	/*
	 * Find a place for the cursor data in display memory.
	 * Must be 64-bit aligned.
	 */
	storage = (scr->gscreen->width*BY2WD*scr->gscreen->r.max.y+7)/8;
	iow32(scr, CurOffset, storage);
	scr->storage = storage*8;

	/*
	 * Cursor goes in the top right corner of the 64x64 array
	 * so the horizontal and vertical presets are 64-16.
	 */
	iow32(scr, CurHVposn, (0<<16)|0);
	iow32(scr, CurHVoff, ((64-16)<<16)|(64-16));

	/*
	 * Load, locate and enable the 64x64 cursor.
	 */
	mach64xxcurload(scr, &arrow);
	mach64xxcurmove(scr, ZP);
	iow32(scr, GenTestCntl, 0x80|r);
}

VGAdev vgamach64xxdev = {
	"mach64xx",

	mach64xxenable,			/* enable */
	0,				/* disable */
	0,				/* page */
	mach64xxlinear,			/* linear */
};

VGAcur vgamach64xxcur = {
	"mach64xxhwgc",

	mach64xxcurenable,		/* enable */
	mach64xxcurdisable,		/* disable */
	mach64xxcurload,		/* load */
	mach64xxcurmove,		/* move */
};
