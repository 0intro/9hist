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

static Lock ark2000pvlock;
static ulong storage;
static Point hotpoint;

static void
setark2000pvpage(int page)
{
	vgaxo(Seqx, 0x15, page);
	vgaxo(Seqx, 0x16, page);
}

static void
disable(void)
{
	uchar seq20;

	seq20 = vgaxi(Seqx, 0x20) & ~0x08;
	vgaxo(Seqx, 0x20, seq20);
}

static void
enable(void)
{
	uchar seq20;

	/*
	 * Disable the cursor then configure for X-Windows style,
	 * 32x32 and 4/8-bit colour depth.
	 * Set cursor colours for 4/8-bit.
	 */
	seq20 = vgaxi(Seqx, 0x20) & ~0x1F;
	vgaxo(Seqx, 0x20, seq20);
	seq20 |= 0x18;

	vgaxo(Seqx, 0x26, Pwhite);
	vgaxo(Seqx, 0x27, Pwhite);
	vgaxo(Seqx, 0x28, Pwhite);
	vgaxo(Seqx, 0x29, Pblack);
	vgaxo(Seqx, 0x2A, Pblack);
	vgaxo(Seqx, 0x2B, Pblack);

	/*
	 * Cursor storage is a 256 byte or 1Kb block located in the last
	 * 16Kb of video memory. Crt25 is the index of which block.
	 */
	storage = (vgaxi(Seqx, 0x10)>>6) & 0x03;
	storage = (1024*1024)<<storage;
	storage -= 256;
	vgaxo(Seqx, 0x25, 0x3F);

	/*
	 * Enable the cursor.
	 */
	vgaxo(Seqx, 0x20, seq20);
}

static void
load(Cursor *c)
{
	uchar *p;
	int x, y;

	/*
	 * Lock the display memory so we can update the
	 * cursor bitmap if necessary.
	 * If it's the same as the last cursor we loaded,
	 * just make sure it's enabled.
	 */
	lock(&ark2000pvlock);
	if(memcmp(c, &curcursor, sizeof(Cursor)) == 0){
		vgaxo(Seqx, 0x20, vgaxi(Seqx, 0x20)|0x08);
		unlock(&ark2000pvlock);
		return;
	}
	memmove(&curcursor, c, sizeof(Cursor));

	/*
	 * Is linear addressing turned on? This will determine
	 * how we access the cursor storage.
	 */
	if(vgaxi(Seqx, 0x10) & 0x10)
		p = ((uchar*)gscreen.base) + storage;
	else {
		setark2000pvpage(storage>>16);
		p = ((uchar*)gscreen.base) + (storage & 0xFFFF);
	}

	/*
	 * The cursor is set in X11 mode which gives the following
	 * truth table:
	 *	and xor	colour
	 *	 0   0	underlying pixel colour
	 *	 0   1	underlying pixel colour
	 *	 1   0	background colour
	 *	 1   1	foreground colour
	 * Put the cursor into the top-left of the 32x32 array.
	 * The manual doesn't say what the data layout in memory is -
	 * this worked out by trial and error.
	 */
	for(y = 0; y < 32; y++){
		for(x = 0; x < 32/8; x++){
			if(x < 16/8 && y < 16){
				*p++ = c->clr[2*y + x]|c->set[2*y + x];
				*p++ = c->set[2*y + x];
			}
			else {
				*p++ = 0x00;
				*p++ = 0x00;
			}
		}
	}

	/*
	 * Set the cursor hotpoint and enable the cursor.
	 */
	hotpoint = c->offset;

	unlock(&ark2000pvlock);
}

static int
move(Point p)
{
	int x, xo, y, yo;

	if(canlock(&ark2000pvlock) == 0)
		return 1;

	/*
	 * Mustn't position the cursor offscreen even partially,
	 * or it might disappear. Therefore, if x or y is -ve, adjust the
	 * cursor origins instead.
	 */
	if((x = p.x+hotpoint.x) < 0){
		xo = -x;
		x = 0;
	}
	else
		xo = 0;
	if((y = p.y+hotpoint.y) < 0){
		yo = -y;
		y = 0;
	}
	else
		yo = 0;

	/*
	 * Load the new values.
	 */
	vgaxo(Seqx, 0x2C, xo);
	vgaxo(Seqx, 0x2D, yo);
	vgaxo(Seqx, 0x21, (x>>8) & 0x0F);
	vgaxo(Seqx, 0x22, x & 0xFF);
	vgaxo(Seqx, 0x23, (y>>8) & 0x0F);
	vgaxo(Seqx, 0x24, y & 0xFF);

	unlock(&ark2000pvlock);
	return 0;
}

static Hwgc ark2000pvhwgc = {
	"ark2000pvhwgc",
	enable,
	load,
	move,
	disable,

	0,
};

static void
ark2000pvpage(int page)
{
	/*
	 * Shouldn't need to lock if linear addressing
	 * is enabled.
	 */
	if((vgaxi(Seqx, 0x10) & 0x10) == 0 && hwgc == &ark2000pvhwgc){
		lock(&ark2000pvlock);
		setark2000pvpage(page);
		unlock(&ark2000pvlock);
	}
	else
		setark2000pvpage(page);
}

static Vgac ark2000pv = {
	"ark2000pv",
	ark2000pvpage,

	0,
};

void
vgaark2000pvlink(void)
{
	addvgaclink(&ark2000pv);
	addhwgclink(&ark2000pvhwgc);
}
