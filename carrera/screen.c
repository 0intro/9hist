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
	uchar	special[12];
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

static ulong rep(ulong, int);

struct{
	Point	pos;
	int	bwid;
}out;

Lock	screenlock;

GBitmap	gscreen =
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

#define IOPORT	((uchar*)0xE0010000)

static uchar
inb(int port)
{
	return IOPORT[port^7];
}

static void
outb(int port, int val)
{
	IOPORT[port^7] = val;
}

void
gborder(GBitmap *l, Rectangle r, int i, Fcode c)
{
	if(i < 0){
		r = inset(r, i);
		i = -i;
	}
	gbitblt(l, r.min, l, Rect(r.min.x, r.min.y, r.max.x, r.min.y+i), c);
	gbitblt(l, Pt(r.min.x, r.max.y-i),
		l, Rect(r.min.x, r.max.y-i, r.max.x, r.max.y), c);
	gbitblt(l, Pt(r.min.x, r.min.y+i),
		l, Rect(r.min.x, r.min.y+i, r.min.x+i, r.max.y-i), c);
	gbitblt(l, Pt(r.max.x-i, r.min.y+i),
		l, Rect(r.max.x-i, r.min.y+i, r.max.x, r.max.y-i), c);
}

void
screenwin(void)
{
	gtexture(&gscreen, gscreen.r, &bgrnd, S);
	w = defont0.info[' '].width;
	h = defont0.height;

	window.min = Pt(100, 100);
	window.max = add(window.min, Pt(10+w*120, 10+h*60));

	gbitblt(&gscreen, window.min, &gscreen, window, Zero);
	window = inset(window, 5);
	cursor = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;

	hwcurs = 0;
}

void	getvmode(VGAmode *v);
void	writeregisters(VGAmode *v);
VGAmode x;

void
screeninit(void)
{
	int i, j;
	uchar *scr;

	setmode(&dfltmode);
	getvmode(&x);
	writeregisters(&x);
return;
	memmove(&arrow, &fatarrow, sizeof(fatarrow));

	scr = (uchar*)EISA(0xC0000);
iprint("%lux\n", scr);
*scr = 0xaa;
i = *scr;
iprint("%2.2ux\n", i);
*scr = 0x55;
i = *scr;
iprint("%2.2ux\n", i);

	for(j = 0; j < 768; j++)
		for(i = 0; i < 1024; i++)
			*(scr+i+(j*1024)) = i;

	/* save space; let bitblt do the conversion work */
	defont = &defont0;
iprint("gbitblt\n");	
	gbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, 0);
iprint("done\n");
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;

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

#ifdef USEME
void
screenputs(char *s, int n)
{
	int i;
	Rune r;
	char buf[4];

	if((getstatus() & IEC) == 0) {
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
#endif

/* replicate (from top) value in v (n bits) until it fills a ulong */
static ulong
rep(ulong v, int n)
{
	int o;
	ulong rv;

	rv = 0;
	for(o = 32 - n; o >= 0; o -= n)
		rv |= (v << o);
	return rv;
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	ulong ans;

	/*
	 * The gnot says 0 is white (max intensity)
	 */
	if(gscreen.ldepth == 0){
		if(p == 0)
				ans = ~0;
		else
				ans = 0;
	}else{
		switch(p){
		case 0:		ans = ~0;		break;
		case 1:		ans = 0xAAAAAAAA;	break;
		case 2:		ans = 0x55555555;	break;
		default:	ans = 0;		break;
		}
	}
	*pr = *pg = *pb = ans;
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p, r, g, b);
	return 0;	/* can't change mono screen colormap */
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
mbbrect(Rectangle r)
{
	USED(r);
}

void
mbbpt(Point p)
{
	USED(p);
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

uchar
grin(ushort i)
{
	EISAOUTB(GRX, i);
	return EISAINB(GR);
}

uchar
arin(ushort i)
{
	uchar junk;
	junk = EISAINB(0x3DA);
	USED(junk);
	EISAOUTB(ARW, i | 0x20);
	return EISAINB(ARR);
}

uchar
crin(ushort i) {
	EISAOUTB(CRX, i);
	return EISAINB(CR);
}

void
getvmode(VGAmode *v)
{
	int i;

	v->general[0] = EISAINB(EMISCR);	/* misc output */
	v->general[1] = EISAINB(EFCR);	/* feature control */
	for(i = 0; i < sizeof(v->sequencer); i++)
		v->sequencer[i] = srin(i);
	for(i = 0; i < sizeof(v->crt); i++) 
		v->crt[i] = crin(i);
	for(i = 0; i < sizeof(v->graphics); i++) 
		v->graphics[i] = grin(i);
	for(i = 0; i < sizeof(v->attribute); i++)
		v->attribute[i] = arin(i);

	v->tseng.viden = EISAINB(0x3c3);
	v->tseng.sr6  = srin(6);
	v->tseng.sr7  = srin(7);
	v->tseng.ar16 = arin(0x16);
	v->tseng.ar17 = arin(0x17);
	v->tseng.crt31= crin(0x31);
	v->tseng.crt32= crin(0x32);
	v->tseng.crt33= crin(0x33);
	v->tseng.crt34= crin(0x34);
	v->tseng.crt35= crin(0x35);
	v->tseng.crt36= crin(0x36);
	v->tseng.crt37= crin(0x37);
}

void
writeregisters(VGAmode *v)
{
	int i;

	print("\t/* general */\n\t");
	for (i=0; i<sizeof(v->general); i++)
		print("0x%.2x, ", v->general[i]);
	print("\n\t/* sequence */\n\t");
	for (i=0; i<sizeof(v->sequencer); i++) {
		if (i>0 && i%8 == 0)
			print("\n\t");
		print("0x%.2x, ", v->sequencer[i]);
	}
	print("\n\t/* crt */\n\t");
	for (i=0; i<sizeof(v->crt); i++) {
		if (i>0 && i%8 == 0)
			print("\n\t");
		print("0x%.2x, ", v->crt[i]);
	}
	print("\n\t/* graphics */\n\t");
	for (i=0; i<sizeof(v->graphics); i++) {
		if (i>0 && i%8 == 0)
			print("\n\t");
		print("0x%.2x, ", v->graphics[i]);
	}
	print("\n\t/* attribute */\n\t");
	for (i=0; i<sizeof(v->attribute); i++) {
		if (i>0 && i%8 == 0)
			print("\n\t");
		print("0x%.2x, ", v->attribute[i]);
	}
	print("\n");
	print("\t/* special */\n");

	for (i=0; i<12; i++) {
		if (i%8 == 0)
			print("\n\t");
		print("0x%.2x, ", ((uchar*)(&v->tseng))[i]);
	}
	print("\n");
}
