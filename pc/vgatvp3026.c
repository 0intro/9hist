#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include <libg.h>
#include "screen.h"
#include "vga.h"

extern Cursor curcursor;

/*
 * TVP3026 Viewpoint Video Interface Pallette.
 * Assumes hooked up to an S3 Vision968.
 */
enum {
	Index		= 0x00,		/* Index */
	Data		= 0x0A,		/* Data */

	CaddrW		= 0x04,		/* Colour Write Address */
	Cdata		= 0x05,		/* Colour Data */

	Cctl		= 0x09,		/* Direct Cursor Control */
	Cram		= 0x0B,		/* Cursor Ram Data */
	Cxlsb		= 0x0C,		/* Cursor X LSB */
	Cxmsb		= 0x0D,		/* Cursor X MSB */
	Cylsb		= 0x0E,		/* Cursor Y LSB */
	Cymsb		= 0x0F,		/* Cursor Y MSB */

	Icctl		= 0x06,		/* Indirect Cursor Control */
};

/*
 * Lower 2-bits of indirect DAC register
 * addressing.
 */
static ushort dacxreg[4] = {
	PaddrW, Pdata, Pixmask, PaddrR
};

static Point hotpoint;

static uchar
tvp3026io(uchar reg, uchar data)
{
	uchar crt55;

	crt55 = vgaxi(Crtx, 0x55) & 0xFC;
	vgaxo(Crtx, 0x55, crt55|((reg>>2) & 0x03));
	vgao(dacxreg[reg & 0x03], data);

	return crt55;
}

static void
tvp3026o(uchar reg, uchar data)
{
	uchar crt55;

	crt55 = tvp3026io(reg, data);
	vgaxo(Crtx, 0x55, crt55);
}

void
tvp3026xo(uchar index, uchar data)
{
	uchar crt55;

	crt55 = tvp3026io(Index, index);
	vgaxo(Crtx, 0x55, crt55|((Data>>2) & 0x03));
	vgao(dacxreg[Data & 0x03], data);
	vgaxo(Crtx, 0x55, crt55);
}

static void
load(Cursor *c)
{
	int x, y;

	/*
	 * Lock the DAC registers so we can update the
	 * cursor bitmap if necessary.
	 * If it's the same as the last cursor we loaded,
	 * just make sure it's enabled.
	 */
	lock(&palettelock);
	if(memcmp(c, &curcursor, sizeof(Cursor)) == 0){
		tvp3026o(Cctl, 0x01);
		unlock(&palettelock);
		return;
	}
	memmove(&curcursor, c, sizeof(Cursor));

	/*
	 * Make sure cursor is off by initialising the cursor
	 * control to defaults.
	 * Write to the indirect control register to make sure
	 * direct register is enabled and upper 2 bits of cursor
	 * RAM address are 0.
	 * The LSBs of the cursor RAM address are in PaddrW.
	 */
	tvp3026xo(Icctl, 0x90);
	tvp3026o(Cctl, 0x00);
	vgao(PaddrW, 0x00);

	/*
	 * Initialise the 64x64 cursor RAM array. There are 2 planes,
	 * p0 and p1. Data is written 8 pixels per byte, with p0 in the
	 * first 512 bytes of the array and p1 in the second.
	 * The cursor is set in 3-colour mode which gives the following
	 * truth table:
	 *	p1 p0	colour
	 *	 0  0	transparent
	 *	 0  1	cursor colour 0
	 *	 1  0	cursor colour 1
	 *	 1  1	cursor colour 2
	 * Put the cursor into the top-left of the 64x64 array.
	 * The 0,0 cursor point is bottom-right, so positioning will
	 * have to take that into account.
	 */
	for(y = 0; y < 64; y++){
		for(x = 0; x < 64/8; x++){
			if(x < 16/8 && y < 16)
				tvp3026o(Cram, c->clr[x+y*2]);
			else
				tvp3026o(Cram, 0x00);
		}
	}
	for(y = 0; y < 64; y++){
		for(x = 0; x < 64/8; x++){
			if(x < 16/8 && y < 16)
				tvp3026o(Cram, c->set[x+y*2]);
			else
				tvp3026o(Cram, 0x00);
		}
	}

	/*
	 * Initialise the cursor hot-point
	 * and enable the cursor in 3-colour mode.
	 */
	hotpoint.x = 64+c->offset.x;
	hotpoint.y = 64+c->offset.y;

	tvp3026o(Cctl, 0x01);

	unlock(&palettelock);
}

static void
enable(void)
{
	lock(&palettelock);

	/*
	 * Make sure cursor is off and direct control enabled.
	 */
	tvp3026xo(Icctl, 0x90);
	tvp3026o(Cctl, 0x00);

	/*
	 * Overscan colour,
	 * cursor colour 1 (white),
	 * cursor colour 2, 3 (black).
	 */
	tvp3026o(CaddrW, 0x00);
	tvp3026o(Cdata, Pwhite); tvp3026o(Cdata, Pwhite); tvp3026o(Cdata, Pwhite);
	tvp3026o(Cdata, Pwhite); tvp3026o(Cdata, Pwhite); tvp3026o(Cdata, Pwhite);
	tvp3026o(Cdata, Pblack); tvp3026o(Cdata, Pblack); tvp3026o(Cdata, Pblack);
	tvp3026o(Cdata, Pblack); tvp3026o(Cdata, Pblack); tvp3026o(Cdata, Pblack);

	/*
	 * Enable the cursor in 3-colour mode.
	 */
	tvp3026o(Cctl, 0x01);

	unlock(&palettelock);
}

static int
move(Point p)
{
	int x, y;

	if(canlock(&palettelock) == 0)
		return 1;

	x = p.x+hotpoint.x;
	y = p.y+hotpoint.y;

	tvp3026o(Cxlsb, x & 0xFF);
	tvp3026o(Cxmsb, (x>>8) & 0x0F);
	tvp3026o(Cylsb, y & 0xFF);
	tvp3026o(Cymsb, (y>>8) & 0x0F);

	unlock(&palettelock);

	return 0;
}

static void
disable(void)
{
	lock(&palettelock);
	tvp3026xo(Icctl, 0x90);
	tvp3026o(Cctl, 0x00);
	unlock(&palettelock);
}

Hwgc tvp3026hwgc = {
	"tvp3026hwgc",
	enable,
	load,
	move,
	disable,

	0,
};

void
vgatvp3026link(void)
{
	addhwgclink(&tvp3026hwgc);
}
