#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	<libg.h>
#include	<gnot.h>
#include	"screen.h"
#include	"vga.h"

#define	MINX	8

extern	GSubfont	defont0;
GSubfont		*defont;

struct{
	Point	pos;
	int	bwid;
}out;

int islittle = 1;		/* little endian bit ordering in bytes */

extern Cursor arrow;
extern uchar cswizzle[256];


/*
 *  screen dimensions
 */
#define MAXX	640
#define MAXY	480

/*
 *  'soft' screen bitmap
 */
GBitmap	gscreen;
GBitmap	vgascreen;

typedef struct VGAmode	VGAmode;
struct VGAmode
{
	uchar	general[2];
	uchar	sequencer[5];
	uchar	crt[0x19];
	uchar	graphics[9];
	uchar	attribute[0x15];
};

/*
 *  640x480 display, 16 bit color.
 */
VGAmode mode12 = 
{
	/* general */
	0xe7, 0x00,
	/* sequence */
	0x03, 0x01, 0x0f, 0x00, 0x06,
	/* crt */
	0x65, 0x4f, 0x50, 0x88, 0x55, 0x9a, 0x09, 0x3e,
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xe8, 0x8b, 0xdf, 0x28, 0x00, 0xe7, 0x04, 0xe3,
	0xff,
	/* graphics */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0f,
	0xff,
	/* attribute */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x3f,
	0x01, 0x10, 0x0f, 0x00, 0x00,
};

static Rectangle mbb;
static Rectangle NULLMBB = {10000, 10000, -10000, -10000};

void
genout(int reg, int val)
{
	if(reg == 0)
		outb(EMISCW, val);
	else if (reg == 1)
		outb(EFCW, val);
}
void
srout(int reg, int val)
{
	outb(SRX, reg);
	outb(SR, val);
}
void
grout(int reg, int val)
{
	outb(GRX, reg);
	outb(GR, val);
}
void
arout(int reg, int val)
{
	inb(0x3DA);
	if (reg <= 0xf) {
		outb(ARW, reg | 0x0);
		outb(ARW, val);
		inb(0x3DA);
		outb(ARW, reg | 0x20);
	} else {
		outb(ARW, reg | 0x20);
		outb(ARW, val);
	}
}

void
crout(int reg, int val)
{
	outb(CRX, reg);
	outb(CR, val);
}

void
setmode(VGAmode *v)
{
	int i;

	for(i = 0; i < sizeof(v->general); i++)
		genout(i, v->general[i]);

	for(i = 0; i < sizeof(v->sequencer); i++)
		srout(i, v->sequencer[i]);

	crout(Cvre, 0);	/* allow writes to CRT registers 0-7 */
	for(i = 0; i < sizeof(v->crt); i++)
		crout(i, v->crt[i]);

	for(i = 0; i < sizeof(v->graphics); i++)
		grout(i, v->graphics[i]);

	for(i = 0; i < sizeof(v->attribute); i++)
		arout(i, v->attribute[i]);
}

void
getmode(VGAmode *v) {
	int i;
	v->general[0] = inb(0x3cc);
	v->general[1] = inb(0x3ca);
	for(i = 0; i < sizeof(v->sequencer); i++) {
		outb(SRX, i);
		v->sequencer[i] = inb(SR);
	}
	for(i = 0; i < sizeof(v->crt); i++) {
		outb(CRX, i);
		v->crt[i] = inb(CR);
	}
	for(i = 0; i < sizeof(v->graphics); i++) {
		outb(GRX, i);
		v->graphics[i] = inb(GR);
	}
	for(i = 0; i < sizeof(v->attribute); i++) {
		inb(0x3DA);
		outb(ARW, i | 0x20);
		v->attribute[i] = inb(ARR);
	}
}

void
printmode(VGAmode *v) {
	int i;
	print("g %2.2x %2x\n",
		v->general[0], v->general[1]);

	print("s ");
	for(i = 0; i < sizeof(v->sequencer); i++) {
		print(" %2.2x", v->sequencer[i]);
	}

	print("\nc ");
	for(i = 0; i < sizeof(v->crt); i++) {
		print(" %2.2x", v->crt[i]);
	}

	print("\ng ");
	for(i = 0; i < sizeof(v->graphics); i++) {
		print(" %2.2x", v->graphics[i]);
	}

	print("\na ");
	for(i = 0; i < sizeof(v->attribute); i++) {
		print(" %2.2x", v->attribute[i]);
	}
	print("\n");
}

void
dumpmodes(void) {
	VGAmode *v;
	int i;

	print("general registers: %02x %02x %02x %02x\n",
		inb(0x3cc), inb(0x3ca), inb(0x3c2), inb(0x3da));

	print("sequence registers: ");
	for(i = 0; i < sizeof(v->sequencer); i++) {
		outb(SRX, i);
		print(" %02x", inb(SR));
	}

	print("\nCRT registers: ");
	for(i = 0; i < sizeof(v->crt); i++) {
		outb(CRX, i);
		print(" %02x", inb(CR));
	}

	print("\nGraphics registers: ");
	for(i = 0; i < sizeof(v->graphics); i++) {
		outb(GRX, i);
		print(" %02x", inb(GR));
	}

	print("\nAttribute registers: ");
	for(i = 0; i < sizeof(v->attribute); i++) {
		inb(0x3DA);
		outb(ARW, i | 0x20);
		print(" %02x", inb(ARR));
	}
}
#endif

void
setscreen(int maxx, int maxy, int ldepth)
{
	int len;

	mbb = NULLMBB;

	/*
	 *  zero hard screen and setup a bitmap for the new size
	 */
	memset((void*)SCREENMEM, 0xff, 64*1024);
	if(ldepth == 3)
		vgascreen.ldepth = 3;
	else
		vgascreen.ldepth = 0;
	vgascreen.base = (void*)SCREENMEM;
	vgascreen.width = (maxx*(1<<vgascreen.ldepth))/32;
	vgascreen.r.max = Pt(maxx, maxy);
	vgascreen.clipr.max = vgascreen.r.max;

	/*
	 *  setup new soft screen, free memory for old screen
	 */
	gscreen.ldepth = ldepth;
	gscreen.width = (maxx*(1<<ldepth))/32;
	gscreen.r.max = Pt(maxx, maxy);
	gscreen.clipr.max = gscreen.r.max;
	len = gscreen.width * BY2WD * maxy;
	if(gscreen.base){
		free(gscreen.base);
		gscreen.base = ((ulong*)smalloc(len+2*1024))+256;
	} else
		gscreen.base = ((ulong*)malloc(len+2*1024))+256;
	memset((char*)gscreen.base, 0xff, len);

	/*
	 *  set string pointer to upper left
	 */
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;
}

void
screeninit(void)
{
	int i, x;
	ulong *l;

	setmode(&mode12);

	/*
	 *  arrow is defined as a big endian
	 */
	bitreverse(arrow.set, 2*16);
	bitreverse(arrow.clr, 2*16);

	/*
	 *  swizzle the font longs.  we do both byte and bit swizzling
	 *  since the font is initialized with big endian longs.
	 */
	defont = &defont0;
	l = defont->bits->base;
	for(i = defont->bits->width*Dy(defont->bits->r); i > 0; i--, l++)
		*l = (*l<<24) | ((*l>>8)&0x0000ff00) | ((*l<<8)&0x00ff0000) | (*l>>24);
	bitreverse((uchar*)defont->bits->base,
		defont->bits->width*BY2WD*Dy(defont->bits->r));

	/*
	 *  set up 'soft' and hard screens
	 */
	if(conf.maxx == 0)
		conf.maxx = MAXX;
	if(conf.maxy == 0)
		conf.maxy = MAXY;
	setscreen(conf.maxx, conf.maxy, conf.ldepth);

	/*
	 *  set up default grey scale color map
	 */
	outb(CMWX, 0);
	for(i = 0; i < 16; i++){
		x = (i*63)/15;
		outb(CM, x);
		outb(CM, x);
		outb(CM, x);
	}
}

/*
 *  collect changes to the 'soft' screen
 */
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
	screenupdate();
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

/*
 *  copy litte endian soft screen to big endian hard screen
 */
void
screenupdate(void)
{
	uchar *sp, *hp;
	int y, len, incs, inch, bits, off;
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

	bits = 1<<vgascreen.ldepth;
	off = (r.min.x*bits)>>(3-vgascreen.ldepth);
	hp = (uchar*)(vgascreen.base+(r.min.y*vgascreen.width)) + off;
	off <<= gscreen.ldepth - vgascreen.ldepth;
	sp = (uchar*)(gscreen.base+(r.min.y*gscreen.width)) + off;
	len = (r.max.x*bits + 7)/8 - (r.min.x*bits)/8;
	if(len <= 0)
		return;

	incs = gscreen.width * BY2WD;
	inch = vgascreen.width * BY2WD;

	switch(gscreen.ldepth){
	case 0:
	case 3:
		/* reverse the bits */
		for (y = r.min.y; y < r.max.y; y++){
			l0update(sp, hp, len);
			sp += incs;
			hp += inch;
		}
		break;
	case 1:
		/* reverse the bits and split into 2 bitmaps */
		for (y = r.min.y; y < r.max.y; y++){
			l1update(sp, hp, len);
			sp += incs;
			hp += inch;
		}
		break;
	}
}

void
screenputnl(void)
{
	Rectangle r;

	out.pos.x = MINX;
	out.pos.y += defont0.height;
	if(out.pos.y > gscreen.r.max.y-defont0.height)
		out.pos.y = gscreen.r.min.y;
	r = Rect(0, out.pos.y, gscreen.r.max.x, out.pos.y+2*defont0.height);
	gbitblt(&gscreen, r.min, &gscreen, r, flipD[0]);
	mbbrect(r);
	screenupdate();
}

Lock screenlock;

void
screenputs(char *s, int n)
{
	Rune r;
	int i;
	Rectangle rs;
	char buf[4];

	rs.min = Pt(0, out.pos.y);
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
		if(r == '\n')
			screenputnl();
		else if(r == '\t'){
			out.pos.x += (8-((out.pos.x-MINX)/out.bwid&7))*out.bwid;
			if(out.pos.x >= gscreen.r.max.x)
				screenputnl();
		}else if(r == '\b'){
			if(out.pos.x >= out.bwid+MINX){
				out.pos.x -= out.bwid;
				gsubfstring(&gscreen, out.pos, defont, " ", flipD[S]);
			}
		}else{
			if(out.pos.x >= gscreen.r.max.x-out.bwid)
				screenputnl();
			out.pos = gsubfstring(&gscreen, out.pos, defont, buf, flipD[S]);
		}
	}
	rs.max = Pt(gscreen.r.max.x, out.pos.y+defont0.height);
	mbbrect(rs);
	screenupdate();
}

int
screenbits(void)
{
	return 1<<gscreen.ldepth;	/* bits per pixel */
}


void
mousescreenupdate(void)
{
	screenupdate();
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	ulong ans;

	/*
	 * The safari monochrome says 0 is black (zero intensity)
	 */
	if(p == 0)
		ans = 0;
	else
		ans = ~0;
	*pr = *pg = *pb = ans;
}


int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p, r, g, b);
	return 0;	/* can't change mono screen colormap */
}

int
hwcursset(uchar *s, uchar *c, int ox, int oy)
{
	USED(s, c, ox, oy);
	return 0;
}

int
hwcursmove(int x, int y)
{
	USED(x, y);
	return 0;
}

void
mouseclock(void)
{
	mouseupdate(1);
}

/*
 *  a fatter than usual cursor for the safari
 */
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

void
bigcursor(void)
{
	memmove(&arrow, &fatarrow, sizeof(fatarrow));
	bitreverse(arrow.set, 2*16);
	bitreverse(arrow.clr, 2*16);
}

/*
 *  Table for separating and reversing bits in a ldepth 1 bitmap.
 *  This aids in coverting a little endian ldepth 1 bitmap into the
 *  2 big-endian ldepth 0 bitmaps used for the VGA bit planes.
 *
 *	if the bits in uchar x are labeled
 *		76543210
 *	then l1revsep[x] yields a ushort with bits
 *		____0246____1357
 *	where _ represents a bit whose value is 0.
 *
 *  This table is used by l1update() in l.s.  l1update is implemented
 *  in assembler for speed (yech).
 *
 */
ushort l1revsep[] = {
	0x0000,	0x0800,	0x0008,	0x0808,	0x0400,	0x0c00,	0x0408,	0x0c08,
	0x0004,	0x0804,	0x000c,	0x080c,	0x0404,	0x0c04,	0x040c,	0x0c0c,
	0x0200,	0x0a00,	0x0208,	0x0a08,	0x0600,	0x0e00,	0x0608,	0x0e08,
	0x0204,	0x0a04,	0x020c,	0x0a0c,	0x0604,	0x0e04,	0x060c,	0x0e0c,
	0x0002,	0x0802,	0x000a,	0x080a,	0x0402,	0x0c02,	0x040a,	0x0c0a,
	0x0006,	0x0806,	0x000e,	0x080e,	0x0406,	0x0c06,	0x040e,	0x0c0e,
	0x0202,	0x0a02,	0x020a,	0x0a0a,	0x0602,	0x0e02,	0x060a,	0x0e0a,
	0x0206,	0x0a06,	0x020e,	0x0a0e,	0x0606,	0x0e06,	0x060e,	0x0e0e,
	0x0100,	0x0900,	0x0108,	0x0908,	0x0500,	0x0d00,	0x0508,	0x0d08,
	0x0104,	0x0904,	0x010c,	0x090c,	0x0504,	0x0d04,	0x050c,	0x0d0c,
	0x0300,	0x0b00,	0x0308,	0x0b08,	0x0700,	0x0f00,	0x0708,	0x0f08,
	0x0304,	0x0b04,	0x030c,	0x0b0c,	0x0704,	0x0f04,	0x070c,	0x0f0c,
	0x0102,	0x0902,	0x010a,	0x090a,	0x0502,	0x0d02,	0x050a,	0x0d0a,
	0x0106,	0x0906,	0x010e,	0x090e,	0x0506,	0x0d06,	0x050e,	0x0d0e,
	0x0302,	0x0b02,	0x030a,	0x0b0a,	0x0702,	0x0f02,	0x070a,	0x0f0a,
	0x0306,	0x0b06,	0x030e,	0x0b0e,	0x0706,	0x0f06,	0x070e,	0x0f0e,
	0x0001,	0x0801,	0x0009,	0x0809,	0x0401,	0x0c01,	0x0409,	0x0c09,
	0x0005,	0x0805,	0x000d,	0x080d,	0x0405,	0x0c05,	0x040d,	0x0c0d,
	0x0201,	0x0a01,	0x0209,	0x0a09,	0x0601,	0x0e01,	0x0609,	0x0e09,
	0x0205,	0x0a05,	0x020d,	0x0a0d,	0x0605,	0x0e05,	0x060d,	0x0e0d,
	0x0003,	0x0803,	0x000b,	0x080b,	0x0403,	0x0c03,	0x040b,	0x0c0b,
	0x0007,	0x0807,	0x000f,	0x080f,	0x0407,	0x0c07,	0x040f,	0x0c0f,
	0x0203,	0x0a03,	0x020b,	0x0a0b,	0x0603,	0x0e03,	0x060b,	0x0e0b,
	0x0207,	0x0a07,	0x020f,	0x0a0f,	0x0607,	0x0e07,	0x060f,	0x0e0f,
	0x0101,	0x0901,	0x0109,	0x0909,	0x0501,	0x0d01,	0x0509,	0x0d09,
	0x0105,	0x0905,	0x010d,	0x090d,	0x0505,	0x0d05,	0x050d,	0x0d0d,
	0x0301,	0x0b01,	0x0309,	0x0b09,	0x0701,	0x0f01,	0x0709,	0x0f09,
	0x0305,	0x0b05,	0x030d,	0x0b0d,	0x0705,	0x0f05,	0x070d,	0x0f0d,
	0x0103,	0x0903,	0x010b,	0x090b,	0x0503,	0x0d03,	0x050b,	0x0d0b,
	0x0107,	0x0907,	0x010f,	0x090f,	0x0507,	0x0d07,	0x050f,	0x0d0f,
	0x0303,	0x0b03,	0x030b,	0x0b0b,	0x0703,	0x0f03,	0x070b,	0x0f0b,
	0x0307,	0x0b07,	0x030f,	0x0b0f,	0x0707,	0x0f07,	0x070f,	0x0f0f,
};
