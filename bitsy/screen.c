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
#include <cursor.h>
#include "screen.h"

enum {
	Wid		= 320,
	Ht		= 240,
	Pal0	= 0x2000,	/* 16-bit pixel data in active mode (12 in passive) */

	hsw		= 0x04,
	elw		= 0x11,
	blw		= 0x0c,

	vsw		= 0x03,
	efw		= 0x01,
	bfw		= 0x0a,

	pcd		= 0x10,
};

struct sa1110fb {
	/* Frame buffer for 16-bit active color */
	short	palette[16];		/* entry 0 set to Pal0, the rest to 0 */
	ushort	pixel[Wid*Ht];		/* Pixel data */
} *framebuf;

enum {
/* LCD Control Register 0, lcd->lccr0 */
	LEN	=  0,	/*  1 bit */
	CMS	=  1,	/*  1 bit */
	SDS	=  2,	/*  1 bit */
	LDM	=  3,	/*  1 bit */
	BAM	=  4,	/*  1 bit */
	ERM	=  5,	/*  1 bit */
	PAS	=  7,	/*  1 bit */
	BLE	=  8,	/*  1 bit */
	DPD	=  9,	/*  1 bit */
	PDD	= 12,	/*  8 bits */
};

enum {
/* LCD Control Register 1, lcd->lccr1 */
	PPL	=  0,	/* 10 bits */
	HSW	= 10,	/*  6 bits */
	ELW	= 16,	/*  8 bits */
	BLW	= 24,	/*  8 bits */
};

enum {
/* LCD Control Register 2, lcd->lccr2 */
	LPP	=  0,	/* 10 bits */
	VSW	= 10,	/*  6 bits */
	EFW	= 16,	/*  8 bits */
	BFW	= 24,	/*  8 bits */
};

enum {
/* LCD Control Register 3, lcd->lccr3 */
	PCD	=  0,	/*  8 bits */
	ACB	=  8,	/*  8 bits */
	API	= 16,	/*  4 bits */
	VSP	= 20,	/*  1 bit */
	HSP	= 21,	/*  1 bit */
	PCP	= 22,	/*  1 bit */
	OEP	= 23,	/*  1 bit */
};

enum {
/* LCD Status Register, lcd->lcsr */
	LFD	=  0,	/*  1 bit */
	BAU	=  1,	/*  1 bit */
	BER	=  2,	/*  1 bit */
	ABC	=  3,	/*  1 bit */
	IOL	=  4,	/*  1 bit */
	IUL	=  5,	/*  1 bit */
	OIU	=  6,	/*  1 bit */
	IUU	=  7,	/*  1 bit */
	OOL	=  8,	/*  1 bit */
	OUL	=  9,	/*  1 bit */
	OOU	= 10,	/*  1 bit */
	OUU	= 11,	/*  1 bit */
};

struct sa1110regs {
	ulong	lccr0;
	ulong	lcsr;
	ulong	dummies[2];
	short*	dbar1;
	ulong	dcar1;
	ulong	dbar2;
	ulong	dcar2;
	ulong	lccr1;
	ulong	lccr2;
	ulong	lccr3;
} *lcd;

Point	ZP = {0, 0};

static Memimage xgscreen =
{
	{ 0, 0, Wid, Ht },	/* r */
	{ 0, 0, Wid, Ht },	/* clipr */
	16,					/* depth */
	1,					/* nchan */
	RGB16,				/* chan */
	nil,				/* cmap */
	nil,				/* data */
	0,					/* zero */
	Wid/2,				/* width */
	0,					/* layer */
	Frepl,				/* flags */
};

Memimage *gscreen;
Memimage *conscol;
Memimage *back;
Memimage *hwcursor;

static void
lcdstop(void) {
	ulong	lccr0;

	lcd->lccr0 &= ~(0<<LEN);	/* disable the LCD */
	while((lcd->lcsr & LDD) == 0)
		sleep(1);
	lcdpower(0);
}

static void
lcdinit(void)
{
	/* map the lcd regs into the kernel's virtual space */
	lcd = (struct sa1110regs*)mapspecial(LCDREGS, sizeof(struct sa1110regs));;

	/* the following works because main memory is direct mapped */
	lcd->lccr0 &= ~(0<<LEN);	/* disable the LCD */
	while((lcd->lcsr & LDD) == 0)
		sleep(1);

	lcd->dbar1 = framebuf->palette;
	lcd->lccr3 = pcd<<PCD | 0<<ACB | 0<<API | 1<<VSP | 1<<HSP | 0<<PCP | 0<<OEP;
	lcd->lccr2 = (Ht-1)<<LPP | vsw<<VSW | efw<<EFW | bfw<<BFW;
	lcd->lccr1 = (Wid-16)<<PPL | hsw<<HSW | elw<<ELW | blw<<BLW;
	lcd->lccr0 = 1<<LEN | 0<<CMS | 0<<SDS | 1<<LDM | 1<<BAM | 1<<ERM | 1<<PAS | 0<<BLE | 0<<DPD | 0<<PDD;

}

void
screeninit(void)
{
	framebuf = xspanalloc(sizeof *framebuf, 0x10, 0);
	memset(framebuf->palette, 0, sizeof framebuf->palette);
	memset(framebuf->pixel, 0xf8, sizeof framebuf->pixel);
	framebuf->palette[0] = Pal0;

	print("LCD status before power up: 0x%lux\n", lcd->lcsr);
	lcdpower(1);
	print("LCD status after power up: 0x%lux\n", lcd->lcsr);
	lcdinit();
	print("LCD status after lcdinit: 0x%lux\n", lcd->lcsr);

	gscreen = &xgscreen;
	gscreen->data = (struct Memdata *)framebuf->pixel;
}

void
flushmemscreen(Rectangle)
{
}

/* 
 * export screen to devdraw
 */
uchar*
attachscreen(Rectangle *r, ulong *chan, int* d, int *width, int *softscreen)
{
	*r = gscreen->r;
	*d = gscreen->depth;
	*chan = gscreen->chan;
	*width = gscreen->width;
	*softscreen = 0;

	return (uchar*)gscreen->data;
}

void
getcolor(ulong p, ulong* pr, ulong* pg, ulong* pb)
{
	USED(p, pr, pg, pb);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p,r,g,b);
	return 0;
}

void
blankscreen(int blank)
{
	USED(blank);
}

void
screenputs(char *s, int n)
{
	USED(s, n);
}
