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
#include "gamma.h"

#define	MINX	8

int landscape = 0;	/* orientation of the screen, default is 0: portait */

enum {
	Wid		= 240,
	Ht		= 320,
	Pal0	= 0x2000,	/* 16-bit pixel data in active mode (12 in passive) */

	hsw		= 0x00,
	elw		= 0x0e,
	blw		= 0x0d,

	vsw		= 0x02,
	efw		= 0x01,
	bfw		= 0x0a,

	pcd		= 0x10,
};

struct sa1110fb {
	/* Frame buffer for 16-bit active color */
	short	palette[16];		/* entry 0 set to Pal0, the rest to 0 */
	ushort	pixel[Wid*Ht];		/* Pixel data */
} *framebuf;

short savedpalette[16];	/* saved during suspend mode */

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
	LDD	=  0,	/*  1 bit */
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

static Memdata xgdata;

static Memimage xgscreen =
{
	{ 0, 0, Wid, Ht },	/* r */
	{ 0, 0, Wid, Ht },	/* clipr */
	16,					/* depth */
	3,					/* nchan */
	RGB16,				/* chan */
	nil,				/* cmap */
	&xgdata,			/* data */
	0,					/* zero */
	Wid/2,				/* width */
	0,					/* layer */
	0,					/* flags */
};

struct{
	Point	pos;
	int	bwid;
}out;

Memimage *gscreen;
Memimage *conscol;
Memimage *back;

Memsubfont	*memdefont;

Lock		screenlock;

Point		ZP = {0, 0};
ushort		*vscreen;	/* virtual screen */
Rectangle	window;
Point		curpos;
int			h, w;
int			drawdebug;

static	ulong	rep(u
