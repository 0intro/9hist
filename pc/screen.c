#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include "screen.h"

static ulong onesbits = ~0;
static Memdata onesdata = {
	nil,
	&onesbits,
};
static Memimage xones = {
	{ 0, 0, 1, 1 },
	{ -100000, -100000, 100000, 100000 },
	3,
	1,
	&onesdata,
	0,
	1
};
Memimage *memones = &xones;

Point ZP = {0, 0};

Memdata gscreendata;
Memimage gscreen;

VGAscr vgascreen[1];

int
screensize(int x, int y, int z)
{
	VGAscr *scr;

	scr = &vgascreen[0];

	/*
	 * BUG: need to check if any xalloc'ed memory needs to
	 * be given back if aperture is set.
	 */
	if(scr->aperture == 0){
		int width = (x*(1<<z))/BI2WD;

		gscreendata.data = xalloc(width*BY2WD*y);
		if(gscreendata.data == 0)
			error("screensize: vga soft memory");
		memset(gscreendata.data, Backgnd, width*BY2WD*y);
		scr->useflush = 1;

		scr->aperture = 0xA0000;
		scr->apsize = 1<<16;
	}
	else
		gscreendata.data = KADDR(scr->aperture);

	gscreen.data = &gscreendata;
	gscreen.ldepth = z;
	gscreen.width = (x*(1<<gscreen.ldepth)+31)/32;
	gscreen.r.min = ZP;
	gscreen.r.max = Pt(x, y);
	gscreen.clipr = gscreen.r;
	gscreen.repl = 0;

	scr->gscreendata = gscreen.data;
	scr->memdefont = getmemdefont();
	scr->gscreen = &gscreen;

//	memset(gscreen.data->data, Backgnd, scr->apsize);

	drawcmap(0);

	return 0;
}

int
screenaperture(int size, int align)
{
	VGAscr *scr;
	ulong aperture;

	scr = &vgascreen[0];

	if(size == 0){
		if(scr->aperture && scr->isupamem)
			upafree(scr->aperture, scr->apsize);
		scr->aperture = 0;
		scr->isupamem = 0;
		return 0;
	}
	if(scr->dev && scr->dev->linear){
		aperture = scr->dev->linear(scr, &size, &align);
		if(aperture == 0)
			return 1;
	}
	else{
		aperture = upamalloc(0, size, align);
		if(aperture == 0)
			return 1;

		if(scr->aperture && scr->isupamem)
			upafree(scr->aperture, scr->apsize);
		scr->isupamem = 1;
	}

	scr->aperture = aperture;
	scr->apsize = size;

	return 0;
}

ulong*
attachscreen(Rectangle* r, int* ld, int* width)
{
	VGAscr *scr;

	scr = &vgascreen[0];
	if(scr->gscreen == nil || scr->gscreendata == nil)
		return nil;

	*r = scr->gscreen->r;
	*ld = scr->gscreen->ldepth;
	*width = scr->gscreen->width;

	return scr->gscreendata->data;
}

void
flushmemscreen(Rectangle r)
{
	VGAscr *scr;
	uchar *sp, *disp, *sdisp, *edisp;
	int y, len, incs, off, page;

	scr = &vgascreen[0];
	if(scr->gscreen == nil || scr->useflush == 0)
		return;
	if(scr->dev == nil || scr->dev->page == nil)
		return;

	if(rectclip(&r, scr->gscreen->r) == 0)
		return;

	incs = scr->gscreen->width * BY2WD;

	switch(scr->gscreen->ldepth){
	default:
		len = 0;
		panic("flushmemscreen: ldepth\n");
		break;
	case 3:
		len = Dx(r);
		break;
	}
	if(len < 1)
		return;

	off = r.min.y*scr->gscreen->width*BY2WD+(r.min.x>>(3-scr->gscreen->ldepth));
	page = off/scr->apsize;
	off %= scr->apsize;
	disp = KADDR(scr->aperture);
	sdisp = disp+off;
	edisp = disp+scr->apsize;

	off = r.min.y*scr->gscreen->width*BY2WD+(r.min.x>>(3-scr->gscreen->ldepth));
	sp = ((uchar*)scr->gscreendata->data) + off;

	scr->dev->page(scr, page);
	for(y = r.min.y; y < r.max.y; y++) {
		if(sdisp + incs < edisp) {
			memmove(sdisp, sp, len);
			sp += incs;
			sdisp += incs;
		}
		else {
			off = edisp - sdisp;
			page++;
			if(off <= len){
				if(off > 0)
					memmove(sdisp, sp, off);
				scr->dev->page(scr, page);
				if(len - off > 0)
					memmove(disp, sp+off, len - off);
			}
			else {
				memmove(sdisp, sp, len);
				scr->dev->page(scr, page);
			}
			sp += incs;
			sdisp += incs - scr->apsize;
		}
	}
}

void
getcolor(ulong p, ulong* pr, ulong* pg, ulong* pb)
{
	VGAscr *scr;
	ulong x;

	scr = &vgascreen[0];
	if(scr->gscreen == nil)
		return;

	switch(scr->gscreen->ldepth){
	default:
		x = 0x0F;
		break;
	case 3:
		x = 0xFF;
		break;
	}
	p &= x;
	p ^= x;

	lock(&cursor);
	*pr = scr->colormap[p][0];
	*pg = scr->colormap[p][1];
	*pb = scr->colormap[p][2];
	unlock(&cursor);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	VGAscr *scr;
	ulong x;

	scr = &vgascreen[0];
	if(scr->gscreen == nil)
		return 0;

	switch(scr->gscreen->ldepth){
	default:
		x = 0x0F;
		break;
	case 3:
		x = 0xFF;
		break;
	}
	p &= x;
	p ^= x;

	lock(&cursor);
	scr->colormap[p][0] = r;
	scr->colormap[p][1] = g;
	scr->colormap[p][2] = b;
	vgao(PaddrW, p);
	vgao(Pdata, r>>(32-6));
	vgao(Pdata, g>>(32-6));
	vgao(Pdata, b>>(32-6));
	unlock(&cursor);

	return ~0;
}

int
cursoron(int dolock)
{
	VGAscr *scr;
	int v;

	scr = &vgascreen[0];
	if(scr->cur == nil || scr->cur->move == nil)
		return 0;

	if(dolock)
		lock(&cursor);
	v = scr->cur->move(scr, mousexy());
	if(dolock)
		unlock(&cursor);

	return v;
}

void
cursoroff(int)
{
}

void
setcursor(Cursor* curs)
{
	VGAscr *scr;

	scr = &vgascreen[0];
	if(scr->cur == nil || scr->cur->load == nil)
		return;

	scr->cur->load(scr, curs);
}
