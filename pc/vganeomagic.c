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

typedef struct {
	int	enable;
	int	x;
	int	y;
	int	colour1;
	int	colour2;
	int	addr;
} CursorNM;

enum {
	CursorMMIO	= 0x1000,	/* MagicMedia 256AV */
};

static ulong
neomagiclinear(VGAscr* scr, int* size, int* align)
{
	ulong aperture, oaperture;
	int oapsize, wasupamem;
	Pcidev *p;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;

	aperture = 0;
	if(p = pcimatch(nil, 0x10C8, 0)){
		switch(p->did){
		case 0x0005:		/* MagicMedia 256AV */
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
neomagicenable(VGAscr* scr)
{
	Pcidev *p;
	Physseg seg;
	int size, align, asize;
	ulong aperture;

	/*
	 * Only once, can't be disabled for now.
	 * scr->io holds the virtual address of
	 * the MMIO registers.
	 */
	if(scr->io)
		return;
	if(p = pcimatch(nil, 0x10C8, 0)){
		switch(p->did){
		case 0x0005:		/* MagicMedia 256AV */
			asize = 2560*1024;
			break;
		default:
			return;
		}
	}
	else
		return;
	scr->io = upamalloc(p->mem[1].bar & ~0x0F, p->mem[1].size, 0);
	if(scr->io == 0)
		return;

	memset(&seg, 0, sizeof(seg));
	seg.attr = SG_PHYSICAL;
	seg.name = smalloc(NAMELEN);
	snprint(seg.name, NAMELEN, "neomagicmmio");
	seg.pa = scr->io;
	seg.size = p->mem[1].size;
	addphysseg(&seg);

	scr->io = (ulong)KADDR(scr->io);

	size = p->mem[0].size;
	align = 0;
	aperture = neomagiclinear(scr, &size, &align);
	if(aperture) {
		scr->aperture = aperture;
		scr->apsize = asize;
		memset(&seg, 0, sizeof(seg));
		seg.attr = SG_PHYSICAL;
		seg.name = smalloc(NAMELEN);
		snprint(seg.name, NAMELEN, "neomagicscreen");
		seg.pa = aperture;
		seg.size = size;
		addphysseg(&seg);
	}
}

static void
neomagiccurdisable(VGAscr* scr)
{
	CursorNM *cursornm;

	if(scr->io == 0)
		return;
	cursornm = KADDR(scr->io+CursorMMIO);
	cursornm->enable = 0;
}

static void
neomagicinitcursor(VGAscr* scr, int xo, int yo, int index)
{
	int x, y;
	uchar *p;
	uint p0, p1;

	p = KADDR(scr->aperture);
	p += scr->storage + index*1024;

	for(y = yo; y < 16; y++){
		p0 = scr->set[2*y];
		p1 = scr->set[2*y+1];
		if(xo){
			p0 = (p0<<xo)|(p1>>(8-xo));
			p1 <<= xo;
		}
		*p++ = p0;
		*p++ = p1;

		for(x = 16; x < 64; x += 8)
			*p++ = 0x00;

		p0 = scr->clr[2*y]|scr->set[2*y];
		p1 = scr->clr[2*y+1]|scr->set[2*y+1];
		if(xo){
			p0 = (p0<<xo)|(p1>>(8-xo));
			p1 <<= xo;
		}
		*p++ = p0;
		*p++ = p1;

		for(x = 16; x < 64; x += 8)
			*p++ = 0x00;
	}
	while(y < 64+yo){
		for(x = 0; x < 64; x += 8){
			*p++ = 0x00;
			*p++ = 0x00;
		}
		y++;
	}
}

static void
neomagiccurload(VGAscr* scr, Cursor* curs)
{
	CursorNM *cursornm;

	if(scr->io == 0)
		return;
	cursornm = KADDR(scr->io+CursorMMIO);

	cursornm->enable = 0;
	memmove(&scr->Cursor, curs, sizeof(Cursor));
	neomagicinitcursor(scr, 0, 0, 0);
	cursornm->enable = 1;
}

static int
neomagiccurmove(VGAscr* scr, Point p)
{
	CursorNM *cursornm;
	int addr, index, x, xo, y, yo;

	if(scr->io == 0)
		return 1;
	cursornm = KADDR(scr->io+CursorMMIO);

	index = 0;
	if((x = p.x+scr->offset.x) < 0){
		xo = -x;
		x = 0;
	}
	else
		xo = 0;
	if((y = p.y+scr->offset.y) < 0){
		yo = -y;
		y = 0;
	}
	else
		yo = 0;

	if(xo || yo){
		index = 1;
		neomagicinitcursor(scr, xo, yo, index);
	}
	addr = ((scr->storage+(1024*index))>>10) & 0xFFF;
	addr = ((addr & 0x00F)<<8)|((addr>>4) & 0xFF);
	if(cursornm->addr != addr)
		cursornm->addr = addr;

	cursornm->x = x;
	cursornm->y = y;

	return 0;
}

static void
neomagiccurenable(VGAscr* scr)
{
	CursorNM *cursornm;

	neomagicenable(scr);
	if(scr->io == 0)
		return;
	cursornm = KADDR(scr->io+CursorMMIO);
	cursornm->enable = 0;

	/*
	 * Cursor colours.
	 */
	cursornm->colour1 = (Pblack<<16)|(Pblack<<8)|Pblack;
	cursornm->colour2 = (Pwhite<<16)|(Pwhite<<8)|Pwhite;

	/*
	 * Find a place for the cursor data in display memory.
	 * 2 cursor images might be needed, 1KB each so use the last
	 * 2KB of the framebuffer and initialise them to be
	 * transparent.
	 */
	scr->storage = /*scr->apsize*/2560*1024-2*1024;

	/*
	 * Load, locate and enable the 64x64 cursor.
	 */
	neomagiccurload(scr, &arrow);
	neomagiccurmove(scr, ZP);
	cursornm->enable = 1;
}

VGAdev vganeomagicdev = {
	"neomagic",

	neomagicenable,
	nil,
	nil,
	neomagiclinear,
};

VGAcur vganeomagiccur = {
	"neomagichwgc",

	neomagiccurenable,
	neomagiccurdisable,
	neomagiccurload,
	neomagiccurmove,
};
