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

#define	MINX	8

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

static Memdata xgdata =
{
	nil,				/* *base */
	nil,				/* *bdata */
	1,					/* ref */
	nil,				/* *imref */
	0,					/* allocd */
};

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

Memimage *gscreen;
Memimage *conscol;
Memimage *back;

Memsubfont	*memdefont;

struct{
	Point	pos;
	int	bwid;
}out;

Lock	screenlock;

Point	ZP = {0, 0};

static Rectangle window;
static Point curpos;
static int h, w;
int drawdebug;

static	ulong	rep(ulong, int);
static	void	screenwin(void);
static	void	screenputc(char *buf);
static	void	scroll(void);

static void
lcdstop(void) {
	lcd->lccr0 &= ~(0<<LEN);	/* disable the LCD */
	while((lcd->lcsr & LDD) == 0)
		delay(10);
	lcdpower(0);
}

static void
lcdinit(void)
{
	lcd->dbar1 = framebuf->palette;
	lcd->lccr3 = pcd<<PCD | 0<<ACB | 0<<API | 1<<VSP | 1<<HSP | 0<<PCP | 0<<OEP;
	lcd->lccr2 = (Ht-1)<<LPP | vsw<<VSW | efw<<EFW | bfw<<BFW;
	lcd->lccr1 = (Wid-16)<<PPL | hsw<<HSW | elw<<ELW | blw<<BLW;
	lcd->lccr0 = 1<<LEN | 0<<CMS | 0<<SDS | 1<<LDM | 1<<BAM | 1<<ERM | 1<<PAS | 0<<BLE | 0<<DPD | 0<<PDD;
}

void
screeninit(void)
{
	int i;

	/* map the lcd regs into the kernel's virtual space */
	lcd = (struct sa1110regs*)mapspecial(LCDREGS, sizeof(struct sa1110regs));;

	framebuf = xspanalloc(sizeof *framebuf, 0x20, 0);
	/* the following works because main memory is direct mapped */

	framebuf->palette[0] = Pal0;

	lcdpower(1);
	lcdinit();

	gscreen = &xgscreen;
	xgdata.bdata = (uchar *)framebuf->pixel;

	i = 0;
	while (i < Wid*Ht*1/3)	framebuf->pixel[i++] = 0xf800;	/* red */
	while (i < Wid*Ht*2/3)	framebuf->pixel[i++] = 0xffff;	/* white */
	while (i < Wid*Ht*3/3)	framebuf->pixel[i++] = 0x001f;	/* blue */

	memimageinit();
	memdefont = getmemdefont();

	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = memdefont->info[' '].width;

	screenwin();
}

void
flushmemscreen(Rectangle)
{
	/* no-op, screen is direct mapped */
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

	return (uchar*)gscreen->data->bdata;
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
	int i;
	Rune r;
	char buf[4];

	if(!islo()) {
		/* don't deadlock trying to print in interrupt */
		if(!canlock(&screenlock))
			return;	
	}
	else
		lock(&screenlock);

	while(n > 0){
		i = chartorune(&r, s);
		if(i == 0){
			s++;
			--n;
			continue;
		}
		memmove(buf, s, i);
		buf[i] = 0;
		n -= i;
		s += i;
		screenputc(buf);
	}
	unlock(&screenlock);
}

static void
screenwin(void)
{
	Point p, q;
	char *greet;
	Memimage *grey;

	memsetchan(gscreen, RGB16);

	back = memwhite;
	conscol = memblack;

	/* a lot of work to get a grey color */
	grey = allocmemimage(Rect(0,0,1,1), RGB16);
	grey->flags |= Frepl;
	grey->clipr = gscreen->r;
	grey->data->bdata[0] = 0x40;
	grey->data->bdata[1] = 0xfd;

	w = memdefont->info[' '].width;
	h = memdefont->height;

	window.min = Pt(4, 4);
	window.max = addpt(window.min, Pt(4+w*33, 4+h*15));

	memimagedraw(gscreen, window, memblack, ZP, memopaque, ZP);
	window = insetrect(window, 4);
	memimagedraw(gscreen, window, memwhite, ZP, memopaque, ZP);

	memimagedraw(gscreen, Rect(window.min.x, window.min.y,
			window.max.x, window.min.y+h+5+6), grey, ZP, nil, ZP);
	freememimage(grey);
	window = insetrect(window, 5);

	greet = " Plan 9 Console ";
	p = addpt(window.min, Pt(10, 0));
	q = memsubfontwidth(memdefont, greet);
	memimagestring(gscreen, p, conscol, ZP, memdefont, greet);
	window.min.y += h+6;
	curpos = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
}

static void
screenputc(char *buf)
{
	Point p;
	int w, pos;
	Rectangle r;
	static int *xp;
	static int xbuf[256];

	if(xp < xbuf || xp >= &xbuf[sizeof(xbuf)])
		xp = xbuf;

	switch(buf[0]) {
	case '\n':
		if(curpos.y+h >= window.max.y)
			scroll();
		curpos.y += h;
		screenputc("\r");
		break;
	case '\r':
		xp = xbuf;
		curpos.x = window.min.x;
		break;
	case '\t':
		p = memsubfontwidth(memdefont, " ");
		w = p.x;
		*xp++ = curpos.x;
		pos = (curpos.x-window.min.x)/w;
		pos = 8-(pos%8);
		r = Rect(curpos.x, curpos.y, curpos.x+pos*w, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min);
		curpos.x += pos*w;
		break;
	case '\b':
		if(xp <= xbuf)
			break;
		xp--;
		r = Rect(*xp, curpos.y, curpos.x, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min);
		curpos.x = *xp;
		break;
	default:
		p = memsubfontwidth(memdefont, buf);
		w = p.x;

		if(curpos.x >= window.max.x-w)
			screenputc("\n");

		*xp++ = curpos.x;
		r = Rect(curpos.x, curpos.y, curpos.x+w, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min);
		memimagestring(gscreen, curpos, conscol, ZP, memdefont, buf);
		curpos.x += w;
	}
}

static void
scroll(void)
{
	int o;
	Point p;
	Rectangle r;

	o = 8*h;
	r = Rpt(window.min, Pt(window.max.x, window.max.y-o));
	p = Pt(window.min.x, window.min.y+o);
	memimagedraw(gscreen, r, gscreen, p, nil, p);
	r = Rpt(Pt(window.min.x, window.max.y-o), window.max);
	memimagedraw(gscreen, r, back, ZP, nil, ZP);

	curpos.y -= o;
}
