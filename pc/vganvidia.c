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

enum {
	Pramin = 0x00710000,
	Pramdac = 0x00680000
};

enum {
	hwCurPos = Pramdac + 0x0300,
	hwCurImage = Pramin + (0x00010000 - 0x0800),
};

static ushort nvidiadid[] = {
	0x0020,		/* Riva TNT */
	0x0028,		/* Riva TNT2 */
	0x0029,		/* Riva TNT2 (Ultra)*/
	0x002C,		/* Riva TNT2 (Vanta) */
	0x002D,		/* Riva TNT2 M64 */
	0x00A0,		/* Riva TNT2 (Integrated) */
	0x0100,		/* GeForce 256 */
	0x0101,		/* GeForce DDR */
	0x0103,		/* Quadro */
	0x0110,		/* GeForce2 MX */
	0x0111,		/* GeForce2 MX DDR */
	0x0112,		/* GeForce 2 Go */
	0x0113,		/* Quadro 2 MXR */
	0x0150,		/* GeForce2 GTS */
	0x0151,		/* GeForce2 GTS (rev 1) */
	0x0152,		/* GeForce2 Ultra */
	0x0153,		/* Quadro 2 Pro */
	0,
};

static Pcidev*
nvidiapci(void)
{
	Pcidev *p;
	ushort *did;

	if((p = pcimatch(nil, 0x10DE, 0)) == nil)
		return nil;
	for(did = nvidiadid; *did; did++){
		if(*did == p->did)
			return p;
	}

	return nil;
}


static ulong
nvidialinear(VGAscr* scr, int* size, int* align)
{
	Pcidev *p;
	int oapsize, wasupamem;
	ulong aperture, oaperture;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;

	aperture = 0;
	if(p = nvidiapci()){
		aperture = p->mem[1].bar & ~0x0F;
		*size = p->mem[1].size;
	}

	if(wasupamem) {
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
nvidiaenable(VGAscr* scr)
{
	Pcidev *p;
	Physseg seg;
	ulong aperture;
	int align, size;

	/*
	 * Only once, can't be disabled for now.
	 * scr->io holds the physical address of
	 * the MMIO registers.
	 */
	if(scr->io)
		return;
	p = nvidiapci();
	if(p == nil)
		return;

	scr->io = upamalloc(p->mem[0].bar & ~0x0F, p->mem[0].size, 0);
	if (scr->io == 0)
		return;

	memset(&seg, 0, sizeof(seg));
	seg.attr = SG_PHYSICAL;
	seg.name = smalloc(NAMELEN);
	snprint(seg.name, NAMELEN, "nvidiammio");
	seg.pa = scr->io;
	seg.size = p->mem[0].size;
	addphysseg(&seg);

	size = p->mem[1].size;
	align = 0;
	aperture = nvidialinear(scr, &size, &align);
	if(aperture) {
		scr->aperture = aperture;
		scr->apsize = size;
		memset(&seg, 0, sizeof(seg));
		seg.attr = SG_PHYSICAL;
		seg.name = smalloc(NAMELEN);
		snprint(seg.name, NAMELEN, "nvidiascreen");
		seg.pa = aperture;
		seg.size = size;
		addphysseg(&seg);
	}
}

static void
nvidiacurdisable(VGAscr* scr)
{
	if(scr->io == 0)
		return;

	vgaxo(Crtx, 0x31, vgaxi(Crtx, 0x31) & ~0x01);
}

static void
nvidiacurload(VGAscr* scr, Cursor* curs)
{
	ulong*	p;
	int		i,j;
	ushort	c,s;
	ulong	tmp;

	if(scr->io == 0)
		return;

	vgaxo(Crtx, 0x31, vgaxi(Crtx, 0x31) & ~0x01);

	p = KADDR(scr->io + hwCurImage);

	for (i=0; i<16; i++) {
		c = (curs->clr[2 * i] << 8) | curs->clr[2 * i+1];
		s = (curs->set[2 * i] << 8) | curs->set[2 * i+1];
		tmp = 0;
		for (j=0; j<16; j++) {
			if(s&0x8000)
				tmp |= 0x80000000;
			else if(c&0x8000)
				tmp |= 0xFFFF0000;
			if (j&0x1) {
				*p++ = tmp;
				tmp = 0;
			} else {
				tmp>>=16;
			}
			c<<=1;
			s<<=1;
		}
		for (j=0; j<8; j++)
			*p++ = 0;
	}
	for (i=0; i<256; i++)
		*p++ = 0;

	scr->offset = curs->offset;
	vgaxo(Crtx, 0x31, vgaxi(Crtx, 0x31) | 0x01);

	return;
}

static int
nvidiacurmove(VGAscr* scr, Point p)
{
	ulong*	cursorpos;

	if(scr->io == 0)
		return 1;

	cursorpos = KADDR(scr->io + hwCurPos);
	*cursorpos = ((p.y+scr->offset.y)<<16)|((p.x+scr->offset.x) & 0xFFFF);

	return 0;
}

static void
nvidiacurenable(VGAscr* scr)
{
	nvidiaenable(scr);
	if(scr->io == 0)
		return;

	vgaxo(Crtx, 0x1F, 0x57);

	nvidiacurload(scr, &arrow);
	nvidiacurmove(scr, ZP);

	vgaxo(Crtx, 0x31, vgaxi(Crtx, 0x31) | 0x01);
}

VGAdev vganvidiadev = {
	"nvidia",

	nvidiaenable,
	nil,
	nil,
	nvidialinear,
};

VGAcur vganvidiacur = {
	"nvidiahwgc",

	nvidiacurenable,
	nvidiacurdisable,
	nvidiacurload,
	nvidiacurmove,
};
