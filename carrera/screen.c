#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

#include	<libg.h>
#include	<gnot.h>

#define	MINX	8

enum
{
	EMISCR=		0x3CC,		/* control sync polarity */
	EMISCW=		0x3C2,
	EFCW=		0x3DA,		/* feature control */
	EFCR=		0x3CA,
	GRX=		0x3CE,		/* index to graphics registers */
	GR=		0x3CF,		/* graphics registers */
	 Grms=		 0x04,		/*  read map select register */
	SRX=		0x3C4,		/* index to sequence registers */
	SR=		0x3C5,		/* sequence registers */
	 Smmask=	 0x02,		/*  map mask */
	CRX=		0x3D4,		/* index to crt registers */
	CR=		0x3D5,		/* crt registers */
	 Cvre=		 0x11,		/*  vertical retrace end */
	ARW=		0x3C0,		/* attribute registers (writing) */
	ARR=		0x3C1,		/* attribute registers (reading) */
	CMRX=		0x3C7,		/* color map read index */
	CMWX=		0x3C8,		/* color map write index */
	CM=		0x3C9,		/* color map data reg */
};

typedef struct VGAmode	VGAmode;
struct VGAmode
{
	uchar	general[2];
	uchar	sequencer[5];
	uchar	crt[0x19];
	uchar	graphics[9];
	uchar	attribute[0x15];
	struct {
		uchar viden;
		uchar sr6;
		uchar sr7;
		uchar ar16;
		uchar ar17;
		uchar crt31;
		uchar crt32;
		uchar crt33;
		uchar crt34;
		uchar crt35;
		uchar crt36;
		uchar crt37;
	} tseng;
};

void	setmode(VGAmode*);

extern	struct GBitmap	gscreen;

VGAmode dfltmode = 
{
	/* general */
	0xef, 0x00, 
	/* sequence */
	0x03, 0x01, 0x0f, 0x00, 0x0e, 
	/* crt */
	0xa1, 0x7f, 0x7f, 0x85, 0x85, 0x96, 0x24, 0xf5, 
	0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x02, 0x88, 0xff, 0x80, 0x60, 0xff, 0x25, 0xab, 
	0xff, 
	/* graphics */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 
	0xff, 
	/* attribute */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 
	0x01, 0x00, 0x0f, 0x00, 0x00,
	/* special */
	0x00, 0x00, 0xbc, 0x00, 0x00, 0x00, 0x28, 0x00, 
	0x0a, 0x00, 0x43, 0x1f, 
};

extern	GSubfont	defont0;
GSubfont		*defont;

Lock	screenlock;

GBitmap	gscreen =
{
	0,
	0,
	(1024*(1<<3))/32,
	3,
	{ 0, 0, 1024, 768 },
	{ 0, 0, 1024, 768 },
	0
};

GBitmap	vgascreen =
{
	EISA(0xA0000),
	0,
	(1024*(1<<3))/32,
	3,
	{ 0, 0, 1024, 768 },
	{ 0, 0, 1024, 768 },
	0
};

uchar bdata[] =
{
	0xC0,
};

GBitmap bgrnd =
{
	(ulong*)bdata,
	0,
	4,
	3,
	{ 0, 0, 1, 1 },
	{ 0, 0, 1, 1 },
	0
};

Cursor fatarrow = {
	{ -1, -1 },
	{
		0xff, 0xff, 0x80, 0x01, 0x80, 0x02, 0x80, 0x0c, 
		0x80, 0x10, 0x80, 0x10, 0x80, 0x08, 0x80, 0x04, 
		0x80, 0x02, 0x80, 0x01, 0x80, 0x02, 0x8c, 0x04, 
		0x92, 0x08, 0x91, 0x10, 0xa0, 0xa0, 0xc0, 0x40, 
	},
	{
		0x00, 0x00, 0x7f, 0xfe, 0x7f, 0xfc, 0x7f, 0xf0, 
		0x7f, 0xe0, 0x7f, 0xe0, 0x7f, 0xf0, 0x7f, 0xf8, 
		0x7f, 0xfc, 0x7f, 0xfe, 0x7f, 0xfc, 0x73, 0xf8, 
		0x61, 0xf0, 0x60, 0xe0, 0x40, 0x40, 0x00, 0x00, 
	},
};

static Rectangle window;
static Point cursor;
static int h, w;
extern Cursor arrow;
static ulong colormap[256][3];
static Rectangle mbb;
static Rectangle NULLMBB = {10000, 10000, -10000, -10000};
static int isscroll;

void
screenwin(void)
{
	gtexture(&gscreen, gscreen.r, &bgrnd, S);
	w = defont0.info[' '].width;
	h = defont0.height;
	defont = &defont0;	

	window.min = Pt(50, 50);
	window.max = add(window.min, Pt(10+w*100, 10+h*40));

	gbitblt(&gscreen, window.min, &gscreen, window, Zero);
	window = inset(window, 5);
	cursor = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	hwcurs = 0;

	mbb = gscreen.r;
	screenupdate();
}

/*
 *  expand 3 and 6 bits of color to 32
 */
static ulong
x3to32(uchar x)
{
	ulong y;

	x = x&7;
	x= (x<<3)|x;
	y = (x<<(32-6))|(x<<(32-12))|(x<<(32-18))|(x<<(32-24))|(x<<(32-30));
	return y;
}

void
screeninit(void)
{
	int i;

	setmode(&dfltmode);
	for(i = 0; i < 256; i++)
		setcolor(i, x3to32(i>>5), x3to32(i>>2), x3to32(i<<1));

	/* allocate a new soft bitmap area */
	gscreen.base = xalloc(1024*1024);

	gbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, 0);

	memmove(&arrow, &fatarrow, sizeof(fatarrow));

	screenwin();
}

static void
scroll(void)
{
	int o;
	Rectangle r;

	o = 5*h;
	r = Rpt(Pt(window.min.x, window.min.y+o), window.max);
	gbitblt(&gscreen, window.min, &gscreen, r, S);
	r = Rpt(Pt(window.min.x, window.max.y-o), window.max);
	gbitblt(&gscreen, r.min, &gscreen, r, Zero);
	cursor.y -= o;
	isscroll = 1;
}

void
mbbrect(Rectangle r)
{
	if (r.min.x < mbb.min.x)
		mbb.min.x = r.min.x;
	if (r.min.y < mbb.min.y)
		mbb.min.y = r.min.y;
	if (r.max.x > mbb.max.x)
		mbb.max.x = r.max.x;
	if (r.max.y > mbb.max.y)
		mbb.max.y = r.max.y;
}

void
mbbpt(Point p)
{
	if (p.x < mbb.min.x)
		mbb.min.x = p.x;
	if (p.y < mbb.min.y)
		mbb.min.y = p.y;
	if (p.x >= mbb.max.x)
		mbb.max.x = p.x+1;
	if (p.y >= mbb.max.y)
		mbb.max.y = p.y+1;
}

static void
screenputc(char *buf)
{
	int pos;

	switch(buf[0]) {
	case '\n':
		if(cursor.y+h >= window.max.y)
			scroll();
		cursor.y += h;
		screenputc("\r");
		break;
	case '\r':
		cursor.x = window.min.x;
		break;
	case '\t':
		pos = (cursor.x-window.min.x)/w;
		pos = 8-(pos%8);
		cursor.x += pos*w;
		break;
	case '\b':
		if(cursor.x-w >= 0)
			cursor.x -= w;
		break;
	default:
		if(cursor.x >= window.max.x-w)
			screenputc("\n");

		cursor = gsubfstring(&gscreen, cursor, &defont0, buf, S);
	}
}

void
screenputs(char *s, int n)
{
	int i;
	Rune r;
	char buf[4];

	if((getstatus() & IE) == 0) {
		/* don't deadlock trying to print in interrupt */
		if(!canlock(&screenlock))
			return;	
	}
	else
		lock(&screenlock);

	mbbpt(cursor);
	while(n > 0) {
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
	if(isscroll) {
		mbb = window;
		isscroll = 0;
	}
	else
		mbbpt(Pt(cursor.x, cursor.y+h));

	screenupdate();
	unlock(&screenlock);
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	p &= (1<<(1<<gscreen.ldepth))-1;
	*pr = colormap[p][0];
	*pg = colormap[p][1];
	*pb = colormap[p][2];
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	p &= (1<<(1<<gscreen.ldepth))-1;
	colormap[p][0] = r;
	colormap[p][1] = g;
	colormap[p][2] = b;
	EISAOUTB(CMWX, 255-p);
	EISAOUTB(CM, r>>(32-6));
	EISAOUTB(CM, g>>(32-6));
	EISAOUTB(CM, b>>(32-6));
	return ~0;
}

void
hwcursset(ulong *s, ulong *c, int ox, int oy)
{
	USED(s, c, ox, oy);
}

void
hwcursmove(int x, int y)
{
	USED(x, y);
}

/* only 1 flavor mouse */
void
mousectl(char *x)
{
	USED(x);
}

/* bits per pixel */
int
screenbits(void)
{
	return 1<<gscreen.ldepth;
}

void
srout(int reg, int val)
{
	EISAOUTB(SRX, reg);
	EISAOUTB(SR, val);
}

uchar
srin(ushort i)
{
        EISAOUTB(SRX, i);
        return EISAINB(SR);
}

void
grout(int reg, int val)
{
	EISAOUTB(GRX, reg);
	EISAOUTB(GR, val);
}

void
genout(int reg, int val)
{
	if(reg == 0)
		EISAOUTB(EMISCW, val);
	else
	if (reg == 1)
		EISAOUTB(EFCW, val);
}

void
arout(int reg, int val)
{
	uchar junk;

	junk = EISAINB(0x3DA);
	USED(junk);

	if (reg <= 0xf) {
		EISAOUTB(ARW, reg | 0x0);
		EISAOUTB(ARW, val);
		junk = EISAINB(0x3DA);
		USED(junk);
		EISAOUTB(ARW, reg | 0x20);
	}
	else {
		EISAOUTB(ARW, reg | 0x20);
		EISAOUTB(ARW, val);
	}
}

void
crout(int reg, int val)
{
	EISAOUTB(CRX, reg);
	EISAOUTB(CR, val);
}

void
setmode(VGAmode *v)
{
	int i;

	/* turn screen off (to avoid damage) */
	srout(1, 0x21);

	EISAOUTB(0x3bf, 0x03);		/* hercules compatibility reg */
	EISAOUTB(0x3d8, 0xa0);		/* display mode control register */
	EISAOUTB(0x3cd, 0x00);		/* segment select */

	srout(0x00, srin(0x00) & 0xFD);	/* synchronous reset*/

	for(i = 0; i < sizeof(v->general); i++)
		genout(i, v->general[i]);

	for(i = 0; i < sizeof(v->sequencer); i++)
		if(i == 1)
			srout(i, v->sequencer[i]|0x20);
		else
			srout(i, v->sequencer[i]);

	/* allow writes to CRT registers 0-7 */
	crout(Cvre, 0);
	for(i = 0; i < sizeof(v->crt); i++)
		crout(i, v->crt[i]);

	for(i = 0; i < sizeof(v->graphics); i++)
		grout(i, v->graphics[i]);

	for(i = 0; i < sizeof(v->attribute); i++)
		arout(i, v->attribute[i]);

	EISAOUTB(0x3C6, 0xFF);	/* pel mask */
	EISAOUTB(0x3C8, 0x00);	/* pel write address */

	EISAOUTB(0x3bf, 0x03);	/* hercules compatibility reg */
	EISAOUTB(0x3d8, 0xa0);	/* display mode control register */

	srout(0x06, v->tseng.sr6);
	srout(0x07, v->tseng.sr7);
	i = EISAINB(0x3da); /* reset flip-flop. inp stat 1*/
	USED(i);
	arout(0x16, v->tseng.ar16);	/* misc */
	arout(0x17, v->tseng.ar17);	/* misc 1*/
	crout(0x31, v->tseng.crt31);	/* extended start. */
	crout(0x32, v->tseng.crt32);	/* extended start. */
	crout(0x33, v->tseng.crt33);	/* extended start. */
	crout(0x34, v->tseng.crt34);	/* stub: 46ee + other bits */
	crout(0x35, v->tseng.crt35);	/* overflow bits */
	crout(0x36, v->tseng.crt36);	/* overflow bits */
	crout(0x37, v->tseng.crt37);	/* overflow bits */
	EISAOUTB(0x3c3, v->tseng.viden);/* video enable */

	/* turn screen on */
	srout(1, v->sequencer[1]);
}

#define swiz(s)	(s<<24)|((s>>8)&0xff00)|((s<<8)&0xff0000)|(s>>24)

void
twizzle(uchar *f, uchar *t)
{
	ulong in1, in2;

	in1 = *(ulong*)f;
	in2 = *(ulong*)(f+4);
	*(ulong*)t = swiz(in2);
	*(ulong*)(t+4) = swiz(in1);
}

void
screenupdate(void)
{
	uchar *sp, *hp, *edisp;
	int i, y, len, off, page, inc;
	Rectangle r;

	r = mbb;
	mbb = NULLMBB;

	if(Dy(r) < 0)
		return;

	if(r.min.x < 0)
		r.min.x = 0;
	if(r.min.y < 0)
		r.min.y = 0;
	if(r.max.x > gscreen.r.max.x)
		r.max.x = gscreen.r.max.x;
	if(r.max.y > gscreen.r.max.y)
		r.max.y = gscreen.r.max.y;

	r.min.x &= ~7;
	len = r.max.x - r.min.x;
	len = (len+7)&~7;
	if(len <= 0)
		return;

	inc = gscreen.width*4;
	off = r.min.y * inc + r.min.x;
	sp = ((uchar*)gscreen.base) + off;

	page = off>>16;
	off &= (1<<16)-1;
	hp = edisp = 0;
	for(y = r.min.y; y < r.max.y; y++){
		if(hp >= edisp){
			hp = ((uchar*)vgascreen.base) + off;
			edisp = ((uchar*)vgascreen.base) + 64*1024;
			EISAOUTB(0x3cd, (page<<4) | page);
			off = r.min.x;
			page++;
		}
		for(i = 0; i < len; i += 8)
			twizzle(sp+i, hp+i);
		hp += inc;
		sp += inc;
	}
}
