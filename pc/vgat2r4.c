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
 * #9 Ticket to Ride IV.
 */
enum {
	IndexLo		= 0x10/4,
	IndexHi		= 0x14/4,
	Data		= 0x18/4,
	IndexCtl	= 0x1C/4,

	Zoom		= 0x54/4,
};

enum {						/* index registers */
	CursorCtl	= 0x30,
	CursorXLo	= 0x31,
	CursorXHi	= 0x32,
	CursorYLo	= 0x33,
	CursorYHi	= 0x34,
	CursorHotX	= 0x35,
	CursorHotY	= 0x36,

	CursorR1	= 0x40,
	CursorG1	= 0x41,
	CursorB1	= 0x42,
	CursorR2	= 0x43,
	CursorG2	= 0x44,
	CursorB2	= 0x45,
	CursorR3	= 0x46,
	CursorG3	= 0x47,
	CursorB3	= 0x48,

	CursorArray	= 0x100,

	CursorMode32x32	= 0x03,
	CursorMode64x64	= 0x07,
	CursorMode	= CursorMode64x64,
};

static ulong
t2r4linear(VGAscr* scr, int* size, int* align)
{
	ulong aperture, oaperture;
	int oapsize, wasupamem;
	Pcidev *p;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;

	aperture = 0;
	if(p = pcimatch(nil, 0x105D, 0)){
		switch(p->did){
		case 0x5348:
			aperture = p->mem[0].bar & ~0x0F;
			*size = p->mem[0].size;
			break;
		default:
			break;
		}
	}

	if(wasupamem){
		if(oaperture == aperture)
			return oaperture;
		upafree(oaperture, oapsize);
	}
	scr->isupamem = 0;

	aperture = upamalloc(aperture, *size, *align);
	if(aperture == 0){
		if(wasupamem && upamalloc(oaperture, oapsize, 0)){
			aperture = oaperture;
			scr->isupamem = 1;
		}
		else
			scr->isupamem = 0;
	}
	else
		scr->isupamem = 1;

	return aperture;
}

static void
t2r4enable(VGAscr* scr)
{
	Pcidev *p;
	Physseg seg;
	int size, align;
	ulong aperture, mmio;

	/*
	 * Only once, can't be disabled for now.
	 * scr->mmio holds the virtual address of
	 * the MMIO registers.
	 */
	if(scr->mmio)
		return;
	if(p = pcimatch(nil, 0x105D, 0)){
		switch(p->did){
		case 0x5348:
			break;
		default:
			return;
		}
	}
	else
		return;
	mmio = upamalloc(p->mem[4].bar & ~0x0F, p->mem[4].size, 0);
	if(mmio == 0)
		return;

	memset(&seg, 0, sizeof(seg));
	seg.attr = SG_PHYSICAL;
	seg.name = smalloc(NAMELEN);
	snprint(seg.name, NAMELEN, "t2r4mmio");
	seg.pa = mmio;
	seg.size = p->mem[4].size;
	addphysseg(&seg);

	scr->mmio = KADDR(mmio);

	size = p->mem[0].size;
	align = 0;
	aperture = t2r4linear(scr, &size, &align);
	if(aperture){
		scr->aperture = aperture;
		scr->apsize = size;
		memset(&seg, 0, sizeof(seg));
		seg.attr = SG_PHYSICAL;
		seg.name = smalloc(NAMELEN);
		snprint(seg.name, NAMELEN, "t2r4screen");
		seg.pa = aperture;
		seg.size = size;
		addphysseg(&seg);
	}
}

static uchar
t2r4xi(VGAscr* scr, int index)
{
	ulong *mmio;

	mmio = scr->mmio;
	mmio[IndexLo] = index & 0xFF;
	mmio[IndexHi] = (index>>8) & 0xFF;

	return mmio[Data];
}

static void
t2r4xo(VGAscr* scr, int index, uchar data)
{
	ulong *mmio;

	mmio = scr->mmio;
	mmio[IndexLo] = index & 0xFF;
	mmio[IndexHi] = (index>>8) & 0xFF;

	mmio[Data] = data;
}

static void
t2r4curdisable(VGAscr* scr)
{
	if(scr->mmio == 0)
		return;
	t2r4xo(scr, CursorCtl, 0x00);
}

static void
t2r4curload(VGAscr* scr, Cursor* curs)
{
	int x, y;
	ulong *mmio;
	uchar *data, p, p0, p1;

	mmio = scr->mmio;
	if(mmio == 0)
		return;

	/*
	 * Make sure cursor is off by initialising the cursor
	 * control to defaults.
	 */
	t2r4xo(scr, CursorCtl, 0x00);

	/*
	 * Set auto-increment mode for index-register addressing
	 * and initialise the cursor array index.
	 */
	mmio[IndexCtl] = 0x01;
	mmio[IndexLo] = CursorArray & 0xFF;
	mmio[IndexHi] = (CursorArray>>8) & 0xFF;

	/*
	 * Initialise the cursor RAM array. There are 2 planes,
	 * p0 and p1. Data is written 4 pixels per byte, with p1 the
	 * MS bit of each pixel.
	 * The cursor is set in X-Windows mode which gives the following
	 * truth table:
	 *	p1 p0	colour
	 *	 0  0	underlying pixel colour
	 *	 0  1	underlying pixel colour
	 *	 1  0	cursor colour 1
	 *	 1  1	cursor colour 2
	 * Put the cursor into the top-left of the array.
	 */
	data = (uchar*)&mmio[Data];
	for(y = 0; y < 32; y++){
		*data = 0xFF;
		*data = 0xAA;
		*data = 0xFF;
		*data = 0xAA;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
		*data = 0x00;
	}
	while(y < 64){
		for(x = 0; x < 64/8; x++){
			*data = 0x00;
			*data = 0x00;
		}
		y++;
	}
	mmio[IndexCtl] = 0x00;

	/*
	 * Initialise the hotpoint and enable the cursor.
	 */
	t2r4xo(scr, CursorHotX, -curs->offset.x);
	t2r4xo(scr, CursorHotY, -curs->offset.y);

	t2r4xo(scr, CursorCtl, CursorMode);
}

static int
t2r4curmove(VGAscr* scr, Point p)
{
	int y, zoom;

	if(scr->mmio == 0)
		return 1;

	t2r4xo(scr, CursorXLo, p.x & 0xFF);
	t2r4xo(scr, CursorXHi, (p.x>>8) & 0x0F);

	zoom = (scr->mmio[Zoom] & 0x0F)+1;
	y = p.y*zoom;
	t2r4xo(scr, CursorYLo, y & 0xFF);
	t2r4xo(scr, CursorYHi, (y>>8) & 0x0F);

	return 0;
}

static void
t2r4curenable(VGAscr* scr)
{
	t2r4enable(scr);
	if(scr->mmio == 0)
		return;

	/*
	 * Make sure cursor is off by initialising the cursor
	 * control to defaults.
	 */
	t2r4xo(scr, CursorCtl, 0x00);

	/*
	 * Cursor colour 1 (white),
	 * cursor colour 2 (black).
	 */
	t2r4xo(scr, CursorR1, Pwhite);
	t2r4xo(scr, CursorG1, Pwhite);
	t2r4xo(scr, CursorB1, Pwhite);

	t2r4xo(scr, CursorR2, Pblack);
	t2r4xo(scr, CursorG2, Pblack);
	t2r4xo(scr, CursorB2, Pblack);

	/*
	 * Load, locate and enable the cursor, 64x64, mode 2.
	 */
	t2r4curload(scr, &arrow);
	t2r4curmove(scr, ZP);
	t2r4xo(scr, CursorCtl, CursorMode);
}

VGAdev vgat2r4dev = {
	"t2r4",

	t2r4enable,
	nil,
	nil,
	t2r4linear,
};

VGAcur vgat2r4cur = {
	"t2r4hwgc",

	t2r4curenable,
	t2r4curdisable,
	t2r4curload,
	t2r4curmove,
};
