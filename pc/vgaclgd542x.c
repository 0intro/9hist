#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include <libg.h>
#include "screen.h"
#include "vga.h"

extern Bitmap gscreen;
extern Cursor curcursor;

static Lock clgd542xlock;
static ulong storage;

static void
clgd542xpage(int page)
{
	lock(&clgd542xlock);
	vgaxo(Grx, 0x09, page<<4);
	unlock(&clgd542xlock);
}

static void
disable(void)
{
	uchar sr12;

	lock(&clgd542xlock);
	sr12 = vgaxi(Seqx, 0x12);
	vgaxo(Seqx, 0x12, sr12 & ~0x01);
	unlock(&clgd542xlock);
}

static void
enable(void)
{
	uchar sr12;
 
	/*
	 * Disable the cursor.
	 */
	lock(&clgd542xlock);
	sr12 = vgaxi(Seqx, 0x12);
	vgaxo(Seqx, 0x12, sr12 & ~0x01);

	/*
	 * Cursor colours.  
	 */
	vgaxo(Seqx, 0x12, sr12|0x02);
	setcolor(0x00, Pwhite<<(32-6), Pwhite<<(32-6), Pwhite<<(32-6));
	setcolor(0x0F, Pblack<<(32-6), Pblack<<(32-6), Pblack<<(32-6));
	vgaxo(Seqx, 0x12, sr12);

	/*
	 * Memory size. The BIOS leaves this in Seq0A, bits 4 and 3.
	 * See Technical Reference Manual Appendix E1, Section 1.3.2.
	 *
	 * The storage area for the 64x64 cursors is the last 16Kb of
	 * display memory.
	 */
	switch((vgaxi(Seqx, 0x0A)>>3) & 0x03){

	case 0:
		storage = (256-16)*1024;
		break;

	case 1:
		storage = (512-16)*1024;
		break;

	case 2:
		storage = (1024-16)*1024;
		break;

	case 3:
		storage = (2048-16)*1024;
		break;
	}
 
	/*
	 * Set the current cursor to index 0
	 * and turn the 64x64 cursor on.
	 */
	vgaxo(Seqx, 0x13, 0);
	vgaxo(Seqx, 0x12, sr12|0x05);

	unlock(&clgd542xlock);
}

static void
initcursor(Cursor* c, int xo, int yo, int index)
{
	uchar *p;
	uint p0, p1;
	int x, y;

	/*
	 * Is linear addressing turned on? This will determine
	 * how we access the cursor storage.
	 */
	if(vgaxi(Seqx, 0x07) & 0xF0)
		p = ((uchar*)gscreen.base) + storage;
	else {
		vgaxo(Grx, 0x09, (storage>>16)<<4);
		p = ((uchar*)gscreen.base) + (storage & 0xFFFF);
	}
	p += index*1024;

	for(y = yo; y < 16; y++){
		p0 = c->clr[2*y];
		p1 = c->clr[2*y+1];
		if(xo){
			p0 = (p0<<xo)|(p1>>(8-xo));
			p1 <<= xo;
		}
		*p++ = p0;
		*p++ = p1;

		for(x = 16; x < 64; x += 8)
			*p++ = 0x00;

		p0 = c->clr[2*y]|c->set[2*y];
		p1 = c->clr[2*y+1]|c->set[2*y+1];
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
load(Cursor* c)
{
	uchar sr12;

	/*
	 * Lock the display memory so we can update the
	 * cursor bitmap if necessary.
	 * Disable the cursor.
	 * If it's the same as the last cursor loaded,
	 * just make sure it's enabled and index 0.
	 */
	lock(&clgd542xlock);
	sr12 = vgaxi(Seqx, 0x12);
	vgaxo(Seqx, 0x12, sr12 & ~0x01);

	if(memcmp(c, &curcursor, sizeof(Cursor)) == 0){
		vgaxo(Seqx, 0x13, 0);
		vgaxo(Seqx, 0x12, sr12|0x05);
		unlock(&clgd542xlock);
		return;
	}
	memmove(&curcursor, c, sizeof(Cursor));
	initcursor(c, 0, 0, 0);

	/*
	 * Enable the cursor.
	 */
	vgaxo(Seqx, 0x13, 0);
	vgaxo(Seqx, 0x12, sr12|0x05);

	unlock(&clgd542xlock);
}

static int
move(Point p)
{
	int index, x, xo, y, yo;

	if(canlock(&clgd542xlock) == 0)
		return 1;

	index = 0;
	if((x = p.x+curcursor.offset.x) < 0){
		xo = -x;
		x = 0;
	}
	else
		xo = 0;
	if((y = p.y+curcursor.offset.y) < 0){
		yo = -y;
		y = 0;
	}
	else
		yo = 0;

	if(xo || yo){
		initcursor(&curcursor, xo, yo, 1);
		index = 1;
	}
	vgaxo(Seqx, 0x13, index<<2);
	
	vgaxo(Seqx, 0x10|((x & 0x07)<<5), (x>>3) & 0xFF);
	vgaxo(Seqx, 0x11|((y & 0x07)<<5), (y>>3) & 0xFF);

	unlock(&clgd542xlock);
	return 0;
}

static Hwgc clgd542xhwgc = {
	"clgd542xhwgc",
	enable,
	load,
	move,
	disable,

	0,
};

static Vgac clgd542x = {
	"clgd542x",
	clgd542xpage,

	0,
};

void
vgaclgd542xlink(void)
{
	addvgaclink(&clgd542x);
	addhwgclink(&clgd542xhwgc);
}
