#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include <libg.h>
#include "screen.h"
#include "vga.h"

/*
 * Hardware graphics cursor support for
 * generic S3 chipset.
 * Assume we're in enhanced mode.
 */
static Lock s3pagelock;
static ulong storage;
static Point hotpoint;

extern Bitmap gscreen;

static void
sets3page(int page)
{
	uchar crt51;

	/*
	 * I don't understand why these are different.
	 */
	if(gscreen.ldepth == 3){
		/*
		 * The S3 registers need to be unlocked for this.
		 * Let's hope they are already:
		 *	vgaxo(Crtx, 0x38, 0x48);
		 *	vgaxo(Crtx, 0x39, 0xA0);
		 *
		 * The page is 6 bits, the lower 4 bits in Crt35<3:0>,
		 * the upper 2 in Crt51<3:2>.
		 */
		vgaxo(Crtx, 0x35, page & 0x0F);
		crt51 = vgaxi(Crtx, 0x51) & 0xF3;
		vgaxo(Crtx, 0x51, crt51|((page & 0x30)>>2));
	}
	else
		vgaxo(Crtx, 0x35, (page<<2) & 0x0C);
}

static void
vsyncactive(void)
{
	/*
	 * Hardware cursor information is fetched from display memory
	 * during the horizontal blank active time. The 80x chips may hang
	 * if the cursor is turned on or off during this period.
	 */
	while((vgai(Status1) & 0x08) == 0)
		;
}

static void
disable(void)
{
	uchar crt45;

	/*
	 * Turn cursor off.
	 */
	crt45 = vgaxi(Crtx, 0x45) & 0xFE;
	vsyncactive();
	vgaxo(Crtx, 0x45, crt45);
}

static void
enable(void)
{
	int i;

	disable();

	/*
	 * Cursor colours. Set both the CR0[EF] and the colour
	 * stack in case we are using a 16-bit RAMDAC.
	 * Why are these colours reversed?
	 */
	vgaxo(Crtx, 0x0E, 0xFF);
	vgaxo(Crtx, 0x0F, 0x00);
	vgaxi(Crtx, 0x45);
	for(i = 0; i < 4; i++)
		vgaxo(Crtx, 0x4A, 0xFF);
	vgaxi(Crtx, 0x45);
	for(i = 0; i < 4; i++)
		vgaxo(Crtx, 0x4B, 0x00);

	/*
	 * Find a place for the cursor data in display memory.
	 * Must be on a 1024-byte boundary.
	 */
	storage = (gscreen.width*BY2WD*gscreen.r.max.y+1023)/1024;
	vgaxo(Crtx, 0x4C, (storage>>8) & 0x0F);
	vgaxo(Crtx, 0x4D, storage & 0xFF);
	storage *= 1024;

	/*
	 * Enable the cursor in X11 mode.
	 */
	vgaxo(Crtx, 0x55, vgaxi(Crtx, 0x55)|0x10);
	vsyncactive();
	vgaxo(Crtx, 0x45, 0x01);
}

static void
load(Cursor *c)
{
	uchar *p;
	int x, y;

	/*
	 * Disable the cursor and lock the display
	 * memory so we can update the cursor bitmap.
	 * Set the display page (do we need to restore
	 * the current contents when done?) and the
	 * pointer to the two planes. What if this crosses
	 * into a new page?
	 */
	disable();
	lock(&s3pagelock);

	sets3page(storage>>16);
	p = ((uchar*)gscreen.base) + (storage & 0xFFFF);

	/*
	 * The cursor is set in X11 mode which gives the following
	 * truth table:
	 *	and xor	colour
	 *	 0   0	underlying pixel colour
	 *	 0   1	underlying pixel colour
	 *	 1   0	background colour
	 *	 1   1	foreground colour
	 * Put the cursor into the top-left of the 64x64 array.
	 */
	for(y = 0; y < 64; y++){
		for(x = 0; x < 64/8; x += 2){
			if(x < 16/8 && y < 16){
				*p++ = c->clr[2*y + x]|c->set[2*y + x];
				*p++ = c->clr[2*y + x+1]|c->set[2*y + x+1];
				*p++ = c->set[2*y + x];
				*p++ = c->set[2*y + x+1];
			}
			else {
				*p++ = 0x00;
				*p++ = 0x00;
				*p++ = 0x00;
				*p++ = 0x00;
			}
		}
	}
	unlock(&s3pagelock);

	/*
	 * Set the cursor hotpoint and enable the cursor.
	 */
	hotpoint = c->offset;
	vsyncactive();
	vgaxo(Crtx, 0x45, 0x01);
}

static int
move(Point p)
{
	int x, xo, y, yo;

	/*
	 * Mustn't position the cursor offscreen even partially,
	 * or it disappears. Therefore, if x or y is -ve, adjust the
	 * cursor offset instead.
	 */
	if((x = p.x+hotpoint.x) < 0){
		xo = -x;
		xo = ((xo+1)/2)*2;
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

	vgaxo(Crtx, 0x46, (x>>8) & 0x07);
	vgaxo(Crtx, 0x47, x & 0xFF);
	vgaxo(Crtx, 0x49, y & 0xFF);
	vgaxo(Crtx, 0x4E, xo);
	vgaxo(Crtx, 0x4F, yo);
	vgaxo(Crtx, 0x48, (y>>8) & 0x07);

	return 0;
}

Hwgc s3hwgc = {
	"s3hwgc",
	enable,
	load,
	move,
	disable,
};

void
s3page(int page)
{
	if(hwgc == &s3hwgc){
		lock(&s3pagelock);
		sets3page(page);
		unlock(&s3pagelock);
	}
	else
		sets3page(page);
}
