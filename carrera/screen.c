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
#include	"screen.h"

#define	MINX	8

#define DAC	((Dac*)BTDac)
typedef struct Dac Dac;
struct Dac
{
	uchar	pad0[7];
	uchar	cr0;
	uchar	pad1[7];
	uchar	cr1;
	uchar	pad2[7];
	uchar	cr2;
	uchar	pad3[7];
	uchar	cr3;
};

char s1[] = { 0x00, 0x00, 0xC0, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	GSubfont*	defont;
extern	GSubfont	defont0;
static	ulong		rep(ulong, int);

struct{
	Point	pos;
	int	bwid;
}out;

Lock	screenlock;

GBitmap	gscreen =
{
	Screenvirt+0x00017924,
	0,
	512,
	3,
	{ 0, 0, 1600, 1240 },
	{ 0, 0, 1600, 1240 },
	0
};

static GBitmap hwcursor=
{
	0,		/* base filled in by malloc when needed */
	0,
	4,
	1,
	{0, 0, 64, 64},
	{0, 0, 64, 64}
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
	Dac *d;
	Point p;
	int i, y;
	ulong zbuf[16];

	memset((void*)Screenvirt, 0xff, 3*1024*1024);

	gtexture(&gscreen, gscreen.r, &bgrnd, S);
	w = defont0.info[' '].width;
	h = defont0.height;

	window.min = Pt(100, 100);
	window.max = add(window.min, Pt(10+w*120, 10+h*60));

	gbitblt(&gscreen, add(window.min, Pt(5, 5)), &gscreen, window, F);
	gbitblt(&gscreen, window.min, &gscreen, window, Zero);
	gborder(&gscreen, window, 4, F);
	window = inset(window, 5);
	for(i = 0; i < h+6; i += 2) {
		y = window.min.y+i;
		gsegment(&gscreen, Pt(window.min.x, y), Pt(window.max.x, y), ~0, F);
	}
	p = add(window.min, Pt(10, 2));
	gsubfstring(&gscreen, p, &defont0, "Brazil Console ", S);
	window.min.y += h+6;
	cursor = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;

	hwcurs = 1;
	d = DAC;
	/* cursor color 1: white */
	d->cr1 = 0x01;
	d->cr0 = 0x81;
	d->cr2 = 0xFF;
	d->cr2 = 0xFF;
	d->cr2 = 0xFF;
	/* cursor color 2: noir */
	d->cr1 = 0x01;
	d->cr0 = 0x82;
	d->cr2 = 0;
	d->cr2 = 0;
	d->cr2 = 0;
	/* cursor color 3: schwarz */
	d->cr1 = 0x01;
	d->cr0 = 0x83;
	d->cr2 = 0;
	d->cr2 = 0;
	d->cr2 = 0;
	/* initialize with all-transparent cursor */
	memset(zbuf, 0, sizeof zbuf);
	hwcursset(zbuf, zbuf, 0, 0);
	/* enable both planes of cursor */
	d->cr1 = 0x03;
	d->cr0 = 0x00;
	d->cr2 = 0xc0;
}

void
dacinit(void)
{
	Dac *d;
	int i;
	ulong r, g, b;

	d = DAC;

	/* Control registers */
	d->cr0 = 0x01;
	d->cr1 = 0x02;
	for(i = 0; i < sizeof s1; i++)
		d->cr2 = s1[i];

	/* Cursor programming */
	d->cr0 = 0x00;
	d->cr1 = 0x03;
	d->cr2 = 0xC0;
	for(i = 0; i < 12; i++)
		d->cr2 = 0;

	/* Load Cursor Ram */
	d->cr0 = 0x00;
	d->cr1 = 0x04;
	for(i = 0; i < 0x400; i++)
		d->cr2 = 0xff;

	for(i = 0; i<256; i++) {
		r = ~rep((i>>5) & 7, 3);
		g = ~rep((i>>2) & 7, 3);
		b = ~rep(i & 3, 2);
		setcolor(i, r, g, b);
	}
	setcolor(85, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA);
	setcolor(170, 0x55555555, 0x55555555, 0x55555555);

	/* Overlay Palette Ram */
	d->cr0 = 0x00;
	d->cr1 = 0x01;
	for(i = 0; i < 0x10; i++) {
		d->cr2 = 0xff;
		d->cr2 = 0xff;
		d->cr2 = 0xff;
	}

	/* Overlay Palette Ram */
	d->cr0 = 0x81;
	d->cr1 = 0x01;
	for(i = 0; i < 3; i++) {
		d->cr2 = 0xff;
		d->cr2 = 0xff;
		d->cr2 = 0xff;
	}
}

void
screeninit(void)
{
	int i;
	ulong r, g, b;

	dacinit();

	memmove(&arrow, &fatarrow, sizeof(fatarrow));

	defont = &defont0;	
	gbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, 0);
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;

	for(i = 0; i<256; i++) {
		r = ~rep((i>>5) & 7, 3);
		g = ~rep((i>>2) & 7, 3);
		b = ~rep(i & 3, 2);
		setcolor(i, r, g, b);
	}
	setcolor(85, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA);
	setcolor(170, 0x55555555, 0x55555555, 0x55555555);

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
	Rectangle r;

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
		if(cursor.x-w >= 0){
			r.min.x = cursor.x-w;
			r.max.x = cursor.x;
			r.min.y = cursor.y;
			r.max.y = cursor.y+defont0.height;
			gbitblt(&gscreen, r.min, &gscreen, r, Zero);
			cursor.x -= w;
		}
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

uchar revtab0[] = {
 0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
 0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
 0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
 0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
 0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
 0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
 0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
 0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
 0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
 0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
 0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
 0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
 0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	Dac *d;
	uchar r, g, b;
	extern uchar revtab0[];

	d = DAC;

	d->cr0 = revtab0[p & 0xFF];
	d->cr1 = 0;
	r = d->cr3;
	g = d->cr3;
	b = d->cr3;
	*pr = (r<<24) | (r<<16) | (r<<8) | r;
	*pg = (g<<24) | (g<<16) | (g<<8) | g;
	*pb = (b<<24) | (b<<16) | (b<<8) | b;
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	Dac *d;
	extern uchar revtab0[];

	d = DAC;

	d->cr0 = revtab0[p & 0xFF];
	d->cr1 = 0;
	d->cr3 = r >> 24;
	d->cr3 = g >> 24;
	d->cr3 = b >> 24;
	return 1;
}

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
hwcursset(ulong *s, ulong *c, int offx, int offy)
{
	Dac *d;
	int x, y;
	Point org;
	uchar ylow, yhigh;
	ulong spix, cpix, dpix;

	hwcursor.base = (ulong *)malloc(1024);
	if(hwcursor.base == 0)
		error(Enomem);
	/* hw cursor is 64x64 with hot point at (32,32) */
	org = add(Pt(32,32), Pt(offx,offy)); 
	for(x = 0; x < 16; x++)
		for(y = 0; y < 16; y++) {
			spix = (s[y]>>(31-(x&0x1F)))&1;
			cpix = (c[y]>>(31-(x&0x1F)))&1;
			dpix = (spix<<1) | cpix;
			gpoint(&hwcursor, add(Pt(x,y), org), dpix, S);
		}

	d = DAC;
	/* have to set y offscreen before writing cursor bits */
	d->cr1 = 0x03;
	d->cr0 = 0x03;
	ylow = d->cr2;
	yhigh = d->cr2;
	d->cr1 = 0x03;
	d->cr0 = 0x03;
	d->cr2 = 0xFF;
	d->cr2 = 0xFF;
	/* now set the bits */
	d->cr1 = 0x04;
	d->cr0 = 0x00;
	for(x = 0; x < 1024; x++)
		d->cr2 = ((uchar *)hwcursor.base)[x];
	/* set y back */
	d->cr1 = 0x03;
	d->cr0 = 0x03;
	d->cr2 = ylow;
	d->cr2 = yhigh;
	free(hwcursor.base);
}

void
hwcursmove(int x, int y)
{
	Dac *d;

	d = DAC;

	x += 380;		/* adjusted by experiment */
	y += 11;		/* adjusted by experiment */
	d->cr1 = 03;
	d->cr0 = 01;
	d->cr2 = x&0xFF;
	d->cr2 = (x>>8)&0xF;
	d->cr2 = y&0xFF;
	d->cr2 = (y>>8)&0xF;
}

int
screenbits(void)
{
	return 1<<gscreen.ldepth;
}

extern	cursorlock(Rectangle);
extern	cursorunlock(void);

/*
 * paste tile into screen.
 * tile is at location r, first pixel in *data. 
 * tl is length of scan line to insert,
 * l is amount to advance data after each scan line.
 */
void
screenload(Rectangle r, uchar *data, int tl, int l)
{
	uchar *q;
	int y, lpart, rpart, mx, m, mr;

	if(!rectclip(&r, gscreen.r) || tl<=0)
		return;

	lock(&screenlock);

	q = gbaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);
	/* may need to do bit insertion on edges */
	if(l == 1){	/* all in one byte */
		if(rpart)
			m &= mr;
		for(y=r.min.y; y<r.max.y; y++){
			*q ^= (*data^*q) & m;
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	}else if(lpart==0 && rpart==0){	/* easy case */
		for(y=r.min.y; y<r.max.y; y++){
			memmove(q, data, tl);
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	}else if(rpart==0){
		for(y=r.min.y; y<r.max.y; y++){
			*q ^= (*data^*q) & m;
			if(tl > 1)
				memmove(q+1, data+1, tl-1);
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	}else if(lpart == 0){
		for(y=r.min.y; y<r.max.y; y++){
			if(tl > 1)
				memmove(q, data, tl-1);
			q[tl-1] ^= (data[tl-1]^q[tl-1]) & mr;
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	}else for(y=r.min.y; y<r.max.y; y++){
			*q ^= (*data^*q) & m;
			if(tl > 2)
				memmove(q+1, data+1, tl-2);
			q[tl-1] ^= (data[tl-1]^q[tl-1]) & mr;
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	unlock(&screenlock);
}
