#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	<libg.h>
#include	<gnot.h>

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

/* imported */
extern	GSubfont defont0;
extern Cursor arrow;

/* exported */
GSubfont *defont;
GBitmap	gscreen;

/* vga screen */
static	Lock	screenlock;
static	GBitmap	vgascreen;
static	ulong	colormap[256][3];
static	Rectangle mbb;
static	Rectangle NULLMBB = {10000, 10000, -10000, -10000};

/* cga screen */
static	int	cga = 1;		/* true if in cga mode */

/* system window */
static	int	isscroll;
static	Rectangle window;
static	int	h, w;
static	Point	cursor;

/*
 *  screen dimensions
 */
#define	MINX	8
#define MAXX	640
#define MAXY	480
#define	CGAWIDTH	160
#define	CGAHEIGHT	24

/*
 *  screen memory addresses
 */
#define SCREENMEM	(0xA0000 | KZERO)
#define CGASCREEN	((uchar*)(0xB8000 | KZERO))

/*
 *  VGA modes
 */
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
 *  640x480 display, 1, 2, or 4 bit color.
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
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x01, 0x10, 0x0f, 0x00, 0x00,
};

/*
 *  320x200 display, 8 bit color.
 */
VGAmode mode13 = 
{
	/* general */
	0x63, 0x00,
	/* sequence */
	0x03, 0x01, 0x0f, 0x00, 0x0e,
	/* crt */
	0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f,
	0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28,
	0x9c, 0x8e, 0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3,
	0xff,
	/* graphics */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f,
	0xff,
	/* attribute */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x41, 0x10, 0x0f, 0x00, 0x00,
};

/*
 *  definitions of known cards
 */
typedef struct Vgacard	Vgacard;
struct Vgacard
{
	char	*name;
	void	(*setpage)(int);	/* routine to page though display memory */
};

enum
{
	Ati,		/* ATI */
	Pvga1a,		/* paradise */
	Trident,	/* Trident 8900 */
	Tseng,		/* tseng labs te4000 */
	Cirrus,		/* Cirrus CLGD542X */
	Generic,
};

static void	nopage(int), tsengpage(int), tridentpage(int), parapage(int);
static void	atipage(int), cirruspage(int);

Vgacard vgachips[] =
{
[Ati]		{ "ati", atipage, },
[Pvga1a]	{ "pvga1a", parapage, },
[Trident]	{ "trident", tridentpage, },
[Tseng]		{ "tseng", tsengpage, },
[Cirrus]	{ "cirrus", cirruspage, },
[Generic]	{ "generic", nopage, },
		{ 0, 0, },
};

Vgacard	*vgacard;	/* current vga card */

/* predefined for the stupid compiler */
static void	setscreen(int, int, int);
static uchar	srin(int);
static void	genout(int, int);
static void	srout(int, int);
static void	grout(int, int);
static void	arout(int, int);
static void	crout(int, int);
static void	setmode(VGAmode*);
static void	getmode(VGAmode*);
static void	printmode(VGAmode*);
static void	cgascreenputc(int);
static void	cgascreenputs(char*, int);
static void	screenputc(char*);
static void	scroll(void);
static ulong	x3to32(uchar);
static ulong	x6to32(uchar);
void	screenupdate(void);
void	pixreverse(uchar*, int, int);
void	mbbrect(Rectangle);

/*
 *  vga device
 */
enum
{
	Qdir=		0,
	Qvgasize=	1,
	Qvgatype=	2,
	Qvgaport=	3,
	Qvgaportw=	4,
	Nvga=		4,
};
Dirtab vgadir[]={
	"vgasize",	{Qvgasize},	0,		0666,
	"vgatype",	{Qvgatype},	0,		0666,
	"vgaport",	{Qvgaport},	0,		0666,
	"vgaportw",	{Qvgaportw},	0,		0666,
};

void
vgareset(void)
{
	vgacard = &vgachips[Generic];
}

void
vgainit(void)
{
}

Chan*
vgaattach(char *upec)
{
	return devattach('v', upec);
}

Chan*
vgaclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
vgawalk(Chan *c, char *name)
{
	return devwalk(c, name, vgadir, Nvga, devgen);
}

void
vgastat(Chan *c, char *dp)
{
	devstat(c, dp, vgadir, Nvga, devgen);
}

Chan*
vgaopen(Chan *c, int omode)
{
	return devopen(c, omode, vgadir, Nvga, devgen);
}

void
vgacreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
vgaclose(Chan *c)
{
	USED(c);
}

long
vgaread(Chan *c, void *buf, long n, ulong offset)
{
	int port;
	uchar *cp;
	char cbuf[64];
	ushort *sp;

	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, vgadir, Nvga, devgen);
	case Qvgasize:
		sprint(cbuf, "%dx%dx%d", gscreen.r.max.x, gscreen.r.max.y,
			1<<gscreen.ldepth);
		return readstr(offset, buf, n, cbuf);
	case Qvgatype:
		return readstr(offset, buf, n, vgacard->name);
	case Qvgaport:
		for (cp = buf, port=offset; port<offset+n; port++)
			*cp++ = inb(port);
		return n;
	case Qvgaportw:
		if((n & 01) || (offset & 01))
			error(Ebadarg);
		n /= 2;
		for (sp = buf, port=offset; port<offset+n; port+=2)
			*sp++ = ins(port);
		return n*2;
	}
	error(Eperm);
	return 0;
}

long
vgawrite(Chan *c, void *buf, long n, ulong offset)
{
	char cbuf[64], *cp;
	Vgacard *vp;
	int port, maxx, maxy, ldepth;
	ushort *sp;

	switch(c->qid.path&~CHDIR){
	case Qdir:
		error(Eperm);
	case Qvgatype:
		if(offset != 0 || n >= sizeof(cbuf) || n < 1)
			error(Ebadarg);
		memmove(cbuf, buf, n);
		cbuf[n] = 0;
		if(cp = strchr(cbuf, '\n'))
			*cp = 0;
		for(vp = vgachips; vp->name; vp++)
			if(strcmp(cbuf, vp->name) == 0){
				vgacard = vp;
				return n;
			}
		error(Ebadarg);
	case Qvgasize:
		if(offset != 0 || n >= sizeof(cbuf))
			error(Ebadarg);
		memmove(cbuf, buf, n);
		cbuf[n] = 0;
		cp = cbuf;
		maxx = strtoul(cp, &cp, 0);
		if(*cp!=0)
			cp++;
		maxy = strtoul(cp, &cp, 0);
		if(*cp!=0)
			cp++;
		switch(strtoul(cp, &cp, 0)){
		case 1:
			ldepth = 0;
			break;
		case 2:
			ldepth = 1;
			break;
		case 4:
			ldepth = 2;
			break;
		case 8:
			ldepth = 3;
			break;
		default:
			ldepth = -1;
		}
		if(maxx == 0 || maxy == 0
		|| maxx > 1280 || maxy > 1024
		|| ldepth > 3 || ldepth < 0)
			error(Ebadarg);
		cursoroff(1);
		setscreen(maxx, maxy, ldepth);
		cursoron(1);
		return n;
	case Qvgaport:
		for (cp = buf, port=offset; port<offset+n; port++)
			outb(port, *cp++);
		return n;
	case Qvgaportw:
		if((n & 01) || (offset & 01))
			error(Ebadarg);
		n /= 2;
		for (sp = buf, port=offset; port<offset+n; port+=2)
			outs(port, *sp++);
		return n*2;
	}
	error(Eperm);
	return 0;
}

void
vgaremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
vgawstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

/*
 *  start the screen in CGA mode.  Create the fonts for VGA.
 */
void
screeninit(void)
{
	int i;
	ulong *l;

	/*
	 *  arrow is defined as a big endian
	 */
	pixreverse(arrow.set, 2*16, 0);
	pixreverse(arrow.clr, 2*16, 0);

	/*
	 *  swizzle the font longs.  we do both byte and bit swizzling
	 *  since the font is initialized with big endian longs.
	 */
	defont = &defont0;
	l = defont->bits->base;
	for(i = defont->bits->width*Dy(defont->bits->r); i > 0; i--, l++)
		*l = (*l<<24) | ((*l>>8)&0x0000ff00) | ((*l<<8)&0x00ff0000) | (*l>>24);
	pixreverse((uchar*)defont->bits->base,
		defont->bits->width*BY2WD*Dy(defont->bits->r), 0);

	/*
	 *  start in CGA mode
	 */
	cga = 1;
	crout(0x0a, 0xff);		/* turn off cursor */
	memset(CGASCREEN, 0, CGAWIDTH*CGAHEIGHT);
}

/*
 *  reconfigure screen shape
 */
static void
setscreen(int maxx, int maxy, int ldepth)
{
	int len, vgamaxy, width, i, x, s;
	ulong *p, *oldp;

	if(ldepth == 3)
		setmode(&mode13);
	else
		setmode(&mode12);

	/* allocate a new soft bitmap area */
	width = (maxx*(1<<ldepth))/32;
	len = width * BY2WD * maxy;
	p = xalloc(len);
	if(p == 0)
		error(Enobitstore);
	memset(p, 0xff, len);
	mbb = NULLMBB;

	/*
	 *  zero hard screen and setup a bitmap for the new size
	 */
	if(ldepth == 3)
		vgascreen.ldepth = 3;
	else
		vgascreen.ldepth = 0;
	vgascreen.width = (maxx*(1<<vgascreen.ldepth))/32;
	if(maxy > (64*1024)/(vgascreen.width*BY2WD))
		vgamaxy = (64*1024)/(vgascreen.width*BY2WD);
	else
		vgamaxy = maxy;
	vgascreen.base = (void*)SCREENMEM;
	vgascreen.r.min = Pt(0, 0);
	vgascreen.r.max = Pt(maxx, vgamaxy);
	vgascreen.clipr = vgascreen.r;

	/*
	 *  setup new soft screen, free memory for old screen
	 */
	oldp = gscreen.base;
	s = splhi();
	gscreen.ldepth = ldepth;
	gscreen.width = width;
	gscreen.r.min = Pt(0, 0);
	gscreen.r.max = Pt(maxx, maxy);
	gscreen.clipr = gscreen.r;
	gscreen.base = p;
	splx(s);
	if(oldp)
		xfree(oldp);

	/*
	 *  default color map
	 */
	switch(ldepth){
	case 3:
		for(i = 0; i < 256; i++)
			setcolor(i, x3to32(i>>5), x3to32(i>>2), x3to32(i<<1));
		break;
	case 2:
	case 1:
	case 0:
		gscreen.ldepth = 3;
		for(i = 0; i < 16; i++){
			x = x6to32((i*63)/15);
			setcolor(i, x, x, x);
		}
		gscreen.ldepth = ldepth;
		break;
	}
	cga = 0;

	/*
	 * display white window
	 */
	mbb = gscreen.r;
	screenupdate();

	/*
	 *  set up inset system window
	 */
	h = defont0.height;
	w = defont0.info[' '].width;

	window.min = Pt(50, 50);
	window.max = add(window.min, Pt(10+w*64, 10+h*24));

	gbitblt(&gscreen, window.min, &gscreen, window, Zero);
	window = inset(window, 5);
	cursor = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	hwcurs = 0;

	mbb = gscreen.r;
	screenupdate();
}

/*
 *   paste tile into soft screen.
 *   tile is at location r, first pixel in *data.  tl is length of scan line to insert,
 *   l is amount to advance data after each scan line.
 */
void
screenload(Rectangle r, uchar *data, int tl, int l)
{
	int y, lpart, rpart, mx, m, mr;
	uchar *q;
	extern void cursorlock(Rectangle);
	extern void cursorunlock(void);

	if(!rectclip(&r, gscreen.r) || tl<=0)
		return;
	lock(&screenlock);
	cursorlock(r);
	screenupdate();
	q = gbaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);
	/* may need to do bit insertion on edges */
	if(l == 1){	/* all in one byte */
		if(rpart)
			m ^= 0xFF >> rpart;
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
			if(l > 2)
				memmove(q+1, data+1, tl-2);
			q[tl-1] ^= (data[tl-1]^q[l-1]) & mr;
			q += gscreen.width*sizeof(ulong);
			data += l;
		}
	mbbrect(r);
	screenupdate();
	cursorunlock();
	screenupdate();
	unlock(&screenlock);
}

/*
 *  copy little endian soft screen to big endian hard screen
 */
void
screenupdate(void)
{
	uchar *sp, *hp, *edisp;
	int y, len, incs, inch, off, page;
	Rectangle r;
	void* (*f)(void*, void*, long);

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

	incs = gscreen.width * BY2WD;
	inch = vgascreen.width * BY2WD;

	switch(gscreen.ldepth){
	case 0:
		r.min.x &= ~15;		/* 16 pixel allignment for l0update() */
		f = l0update;
		len = (r.max.x + 7)/8 - r.min.x/8;
		if(len & 1)
			len++;		/* 16 bit allignment for l0update() */
		break;
	case 1:
		r.min.x &= ~15;		/* 8 pixel allignment for l1update() */
		f = l1update;
		len = (r.max.x + 7)/8 - r.min.x/8;
		if(len & 1)
			len++;		/* 8 pixel allignment for l1update() */
		break;
	case 2:
		r.min.x &= ~7;		/* 8 pixel allignment for l2update() */
		f = l2update;
		len = (r.max.x + 7)/8 - r.min.x/8;
		break;
	case 3:
		f = memmove;
		len = r.max.x - r.min.x;
		break;
	default:
		return;
	}
	if(len < 1)
		return;

	off = r.min.y * vgascreen.width * BY2WD + (r.min.x>>(3 - vgascreen.ldepth));
	page = off>>16;
	off &= (1<<16)-1;
	hp = ((uchar*)vgascreen.base) + off;
	off = r.min.y * gscreen.width * BY2WD
		+ (r.min.x>>(3 - gscreen.ldepth));
	sp = ((uchar*)gscreen.base) + off;

	edisp = ((uchar*)vgascreen.base) + 64*1024;
	vgacard->setpage(page);
	for(y = r.min.y; y < r.max.y; y++){
		if(hp + inch < edisp){
			f(hp, sp, len);
			sp += incs;
			hp += inch;
		} else {
			off = edisp - hp;
			if(off <= len){
				if(off > 0)
					f(hp, sp, off);
				vgacard->setpage(++page);
				if(len - off > 0)
					f(vgascreen.base, sp+off, len - off);
			} else {
				f(hp, sp, len);
				vgacard->setpage(++page);
			}
			sp += incs;
			hp += inch - 64*1024;
		}
	}
}

void
screenputs(char *s, int n)
{
	int i;
	Rune r;
	char buf[4];

	if(cga) {
		cgascreenputs(s, n);
		return;
	}

	if((getstatus() & IFLAG) == 0) {
		/* don't deadlock trying to print in interrupt */
		if(!canlock(&screenlock))
			return;	
	} else
		lock(&screenlock);

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

	screenupdate();
	unlock(&screenlock);
}

/*
 *  accessing registers
 */
static uchar
srin(int i) {
	outb(SRX, i);
	return inb(SR);
}
static void
genout(int reg, int val)
{
	if(reg == 0)
		outb(EMISCW, val);
	else if (reg == 1)
		outb(EFCW, val);
}
static void
srout(int reg, int val)
{
	outb(SRX, reg);
	outb(SR, val);
}
static void
grout(int reg, int val)
{
	outb(GRX, reg);
	outb(GR, val);
}
static void
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
static void
crout(int reg, int val)
{
	outb(CRX, reg);
	outb(CR, val);
}

static void
setmode(VGAmode *v)
{
	int i;

	/* turn screen off (to avoid damage) */
	srout(1, 0x21);

	for(i = 0; i < sizeof(v->general); i++)
		genout(i, v->general[i]);

	for(i = 0; i < sizeof(v->sequencer); i++)
		if(i == 1)
			srout(i, v->sequencer[i]|0x20);		/* avoid enabling screen */
		else
			srout(i, v->sequencer[i]);

	crout(Cvre, 0);	/* allow writes to CRT registers 0-7 */
	for(i = 0; i < sizeof(v->crt); i++)
		crout(i, v->crt[i]);

	for(i = 0; i < sizeof(v->graphics); i++)
		grout(i, v->graphics[i]);

	for(i = 0; i < sizeof(v->attribute); i++)
		arout(i, v->attribute[i]);

	/* turn screen on */
	srout(1, v->sequencer[1]);
}

static void
getmode(VGAmode *v)
{
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

static void
printmode(VGAmode *v)
{
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
static ulong
x6to32(uchar x)
{
	ulong y;

	x = x&0x3f;
	y = (x<<(32-6))|(x<<(32-12))|(x<<(32-18))|(x<<(32-24))|(x<<(32-30));
	return y;
}

/*
 *  just in case we have one
 */
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
 *  paging routines for different cards
 */
static void
nopage(int page)
{
	USED(page);
}

/*
 * Extended registers can be read with inb(), but must be
 * written with outs(). The index must be written each time
 * before the register is accessed.
 * The page bits are spread across registers 0xAE and 0xB2.
 * This can go away when we use the memory aperture.
 */
static void
atipage(int page)
{
	/* the ext register is in the ATI ROM at a fixed address */
	ushort extreg = *((ushort *)0x800C0010);
	uchar v;

	outb(extreg, 0xAE);
	v = (inb(extreg+1) & 0xFC)|((page>>4) & 0x03);
	outs(extreg, (v<<8)|0xAE);

	outb(extreg, 0xB2);
	v = (inb(extreg+1) & 0xE1)|((page & 0x0F)<<1);
	outs(extreg, (v<<8)|0xB2);
}

/*
 * The following assumes that the new mode registers have been selected.
 */
static void
tridentpage(int page)
{
	srout(0xe, (srin(0xe)&0xf0) | page^0x2);
}
static void
tsengpage(int page)
{
	outb(0x3cd, (page<<4)|page);
}
static void
cirruspage(int page)
{
	grout(0x9, page<<4);
}
static void
parapage(int page)
{
	grout(0x9, page<<4);
}

void
mousescreenupdate(void)
{
	if(canlock(&screenlock) == 0)
		return;

	screenupdate();
	unlock(&screenlock);
}

static void
cgascreenputc(int c)
{
	int i;
	static int color;
	static int pos;

	if(c == '\n'){
		pos = pos/CGAWIDTH;
		pos = (pos+1)*CGAWIDTH;
	} else if(c == '\t'){
		i = 8 - ((pos/2)&7);
		while(i-->0)
			cgascreenputc(' ');
	} else if(c == '\b'){
		if(pos >= 2)
			pos -= 2;
		cgascreenputc(' ');
		pos -= 2;
	} else {
		CGASCREEN[pos++] = c;
		CGASCREEN[pos++] = 2;	/* green on black */
	}
	if(pos >= CGAWIDTH*CGAHEIGHT){
		memmove(CGASCREEN, &CGASCREEN[CGAWIDTH], CGAWIDTH*(CGAHEIGHT-1));
		memset(&CGASCREEN[CGAWIDTH*(CGAHEIGHT-1)], 0, CGAWIDTH);
		pos = CGAWIDTH*(CGAHEIGHT-1);
	}
}

static void
cgascreenputs(char *s, int n)
{
	while(n-- > 0)
		cgascreenputc(*s++);
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

static void
screenputc(char *buf)
{
	int pos;

	mbbpt(cursor);
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
		if(cursor.x-w >= window.min.x)
			cursor.x -= w;
		break;
	default:
		if(cursor.x >= window.max.x-w)
			screenputc("\n");

		cursor = gsubfstring(&gscreen, cursor, &defont0, buf, S);
	}
	mbbpt(Pt(cursor.x, cursor.y+h));
}

int
screenbits(void)
{
	return 1<<gscreen.ldepth;	/* bits per pixel */
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	p &= (1<<(1<<gscreen.ldepth))-1;
	lock(&screenlock);
	*pr = colormap[p][0];
	*pg = colormap[p][1];
	*pb = colormap[p][2];
	unlock(&screenlock);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	p &= (1<<(1<<gscreen.ldepth))-1;
	lock(&screenlock);
	colormap[p][0] = r;
	colormap[p][1] = g;
	colormap[p][2] = b;
	outb(CMWX, p);
	outb(CM, r>>(32-6));
	outb(CM, g>>(32-6));
	outb(CM, b>>(32-6));
	unlock(&screenlock);
	return ~0;
}

/*
 *  Table for separating and reversing bits in a ldepth 1 bitmap.
 *  This aids in coverting a little endian ldepth 1 bitmap into the
 *  2 big-endian ldepth 0 bitmaps used for the VGA bit planes.
 *
 *	if the bits in uchar x are labeled
 *		76543210
 *	then l1revsep[x] yields a ulong with bits
 *		________1357________0246
 *	where _ represents a bit whose value is 0.
 *
 *  This table is used by l1update() in l.s.  l1update is implemented
 *  in assembler for speed.
 *
 */
ulong l1revsep[] = {
 0x00000, 0x00008, 0x80000, 0x80008, 0x00004, 0x0000c, 0x80004, 0x8000c,
 0x40000, 0x40008, 0xc0000, 0xc0008, 0x40004, 0x4000c, 0xc0004, 0xc000c,
 0x00002, 0x0000a, 0x80002, 0x8000a, 0x00006, 0x0000e, 0x80006, 0x8000e,
 0x40002, 0x4000a, 0xc0002, 0xc000a, 0x40006, 0x4000e, 0xc0006, 0xc000e,
 0x20000, 0x20008, 0xa0000, 0xa0008, 0x20004, 0x2000c, 0xa0004, 0xa000c,
 0x60000, 0x60008, 0xe0000, 0xe0008, 0x60004, 0x6000c, 0xe0004, 0xe000c,
 0x20002, 0x2000a, 0xa0002, 0xa000a, 0x20006, 0x2000e, 0xa0006, 0xa000e,
 0x60002, 0x6000a, 0xe0002, 0xe000a, 0x60006, 0x6000e, 0xe0006, 0xe000e,
 0x00001, 0x00009, 0x80001, 0x80009, 0x00005, 0x0000d, 0x80005, 0x8000d,
 0x40001, 0x40009, 0xc0001, 0xc0009, 0x40005, 0x4000d, 0xc0005, 0xc000d,
 0x00003, 0x0000b, 0x80003, 0x8000b, 0x00007, 0x0000f, 0x80007, 0x8000f,
 0x40003, 0x4000b, 0xc0003, 0xc000b, 0x40007, 0x4000f, 0xc0007, 0xc000f,
 0x20001, 0x20009, 0xa0001, 0xa0009, 0x20005, 0x2000d, 0xa0005, 0xa000d,
 0x60001, 0x60009, 0xe0001, 0xe0009, 0x60005, 0x6000d, 0xe0005, 0xe000d,
 0x20003, 0x2000b, 0xa0003, 0xa000b, 0x20007, 0x2000f, 0xa0007, 0xa000f,
 0x60003, 0x6000b, 0xe0003, 0xe000b, 0x60007, 0x6000f, 0xe0007, 0xe000f,
 0x10000, 0x10008, 0x90000, 0x90008, 0x10004, 0x1000c, 0x90004, 0x9000c,
 0x50000, 0x50008, 0xd0000, 0xd0008, 0x50004, 0x5000c, 0xd0004, 0xd000c,
 0x10002, 0x1000a, 0x90002, 0x9000a, 0x10006, 0x1000e, 0x90006, 0x9000e,
 0x50002, 0x5000a, 0xd0002, 0xd000a, 0x50006, 0x5000e, 0xd0006, 0xd000e,
 0x30000, 0x30008, 0xb0000, 0xb0008, 0x30004, 0x3000c, 0xb0004, 0xb000c,
 0x70000, 0x70008, 0xf0000, 0xf0008, 0x70004, 0x7000c, 0xf0004, 0xf000c,
 0x30002, 0x3000a, 0xb0002, 0xb000a, 0x30006, 0x3000e, 0xb0006, 0xb000e,
 0x70002, 0x7000a, 0xf0002, 0xf000a, 0x70006, 0x7000e, 0xf0006, 0xf000e,
 0x10001, 0x10009, 0x90001, 0x90009, 0x10005, 0x1000d, 0x90005, 0x9000d,
 0x50001, 0x50009, 0xd0001, 0xd0009, 0x50005, 0x5000d, 0xd0005, 0xd000d,
 0x10003, 0x1000b, 0x90003, 0x9000b, 0x10007, 0x1000f, 0x90007, 0x9000f,
 0x50003, 0x5000b, 0xd0003, 0xd000b, 0x50007, 0x5000f, 0xd0007, 0xd000f,
 0x30001, 0x30009, 0xb0001, 0xb0009, 0x30005, 0x3000d, 0xb0005, 0xb000d,
 0x70001, 0x70009, 0xf0001, 0xf0009, 0x70005, 0x7000d, 0xf0005, 0xf000d,
 0x30003, 0x3000b, 0xb0003, 0xb000b, 0x30007, 0x3000f, 0xb0007, 0xb000f,
 0x70003, 0x7000b, 0xf0003, 0xf000b, 0x70007, 0x7000f, 0xf0007, 0xf000f,
};

/*
 *  Table for separating and reversing bits in a ldepth 2 bitmap.
 *  This aids in coverting a little endian ldepth 1 bitmap into the
 *  4 big-endian ldepth 0 bitmaps used for the VGA bit planes.
 *
 *	if the bits in uchar x are labeled
 *		76543210
 *	then l1revsep[x] yields a ulongt with bits
 *		______37______26______15______04
 *	where _ represents a bit whose value is 0.
 *
 *  This table is used by l2update() in l.s.  l2update is implemented
 *  in assembler for speed.
 *
 */
ulong l2revsep[] = {
 0x0000000, 0x0000002, 0x0000200, 0x0000202, 0x0020000, 0x0020002, 0x0020200, 0x0020202,
 0x2000000, 0x2000002, 0x2000200, 0x2000202, 0x2020000, 0x2020002, 0x2020200, 0x2020202,
 0x0000001, 0x0000003, 0x0000201, 0x0000203, 0x0020001, 0x0020003, 0x0020201, 0x0020203,
 0x2000001, 0x2000003, 0x2000201, 0x2000203, 0x2020001, 0x2020003, 0x2020201, 0x2020203,
 0x0000100, 0x0000102, 0x0000300, 0x0000302, 0x0020100, 0x0020102, 0x0020300, 0x0020302,
 0x2000100, 0x2000102, 0x2000300, 0x2000302, 0x2020100, 0x2020102, 0x2020300, 0x2020302,
 0x0000101, 0x0000103, 0x0000301, 0x0000303, 0x0020101, 0x0020103, 0x0020301, 0x0020303,
 0x2000101, 0x2000103, 0x2000301, 0x2000303, 0x2020101, 0x2020103, 0x2020301, 0x2020303,
 0x0010000, 0x0010002, 0x0010200, 0x0010202, 0x0030000, 0x0030002, 0x0030200, 0x0030202,
 0x2010000, 0x2010002, 0x2010200, 0x2010202, 0x2030000, 0x2030002, 0x2030200, 0x2030202,
 0x0010001, 0x0010003, 0x0010201, 0x0010203, 0x0030001, 0x0030003, 0x0030201, 0x0030203,
 0x2010001, 0x2010003, 0x2010201, 0x2010203, 0x2030001, 0x2030003, 0x2030201, 0x2030203,
 0x0010100, 0x0010102, 0x0010300, 0x0010302, 0x0030100, 0x0030102, 0x0030300, 0x0030302,
 0x2010100, 0x2010102, 0x2010300, 0x2010302, 0x2030100, 0x2030102, 0x2030300, 0x2030302,
 0x0010101, 0x0010103, 0x0010301, 0x0010303, 0x0030101, 0x0030103, 0x0030301, 0x0030303,
 0x2010101, 0x2010103, 0x2010301, 0x2010303, 0x2030101, 0x2030103, 0x2030301, 0x2030303,
 0x1000000, 0x1000002, 0x1000200, 0x1000202, 0x1020000, 0x1020002, 0x1020200, 0x1020202,
 0x3000000, 0x3000002, 0x3000200, 0x3000202, 0x3020000, 0x3020002, 0x3020200, 0x3020202,
 0x1000001, 0x1000003, 0x1000201, 0x1000203, 0x1020001, 0x1020003, 0x1020201, 0x1020203,
 0x3000001, 0x3000003, 0x3000201, 0x3000203, 0x3020001, 0x3020003, 0x3020201, 0x3020203,
 0x1000100, 0x1000102, 0x1000300, 0x1000302, 0x1020100, 0x1020102, 0x1020300, 0x1020302,
 0x3000100, 0x3000102, 0x3000300, 0x3000302, 0x3020100, 0x3020102, 0x3020300, 0x3020302,
 0x1000101, 0x1000103, 0x1000301, 0x1000303, 0x1020101, 0x1020103, 0x1020301, 0x1020303,
 0x3000101, 0x3000103, 0x3000301, 0x3000303, 0x3020101, 0x3020103, 0x3020301, 0x3020303,
 0x1010000, 0x1010002, 0x1010200, 0x1010202, 0x1030000, 0x1030002, 0x1030200, 0x1030202,
 0x3010000, 0x3010002, 0x3010200, 0x3010202, 0x3030000, 0x3030002, 0x3030200, 0x3030202,
 0x1010001, 0x1010003, 0x1010201, 0x1010203, 0x1030001, 0x1030003, 0x1030201, 0x1030203,
 0x3010001, 0x3010003, 0x3010201, 0x3010203, 0x3030001, 0x3030003, 0x3030201, 0x3030203,
 0x1010100, 0x1010102, 0x1010300, 0x1010302, 0x1030100, 0x1030102, 0x1030300, 0x1030302,
 0x3010100, 0x3010102, 0x3010300, 0x3010302, 0x3030100, 0x3030102, 0x3030300, 0x3030302,
 0x1010101, 0x1010103, 0x1010301, 0x1010303, 0x1030101, 0x1030103, 0x1030301, 0x1030303,
 0x3010101, 0x3010103, 0x3010301, 0x3010303, 0x3030101, 0x3030103, 0x3030301, 0x3030303,
};

/*
 *  reverse pixels into little endian order (also used by l0update)
 */
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
uchar revtab1[] = {
 0x00, 0x40, 0x80, 0xc0, 0x10, 0x50, 0x90, 0xd0,
 0x20, 0x60, 0xa0, 0xe0, 0x30, 0x70, 0xb0, 0xf0,
 0x04, 0x44, 0x84, 0xc4, 0x14, 0x54, 0x94, 0xd4,
 0x24, 0x64, 0xa4, 0xe4, 0x34, 0x74, 0xb4, 0xf4,
 0x08, 0x48, 0x88, 0xc8, 0x18, 0x58, 0x98, 0xd8,
 0x28, 0x68, 0xa8, 0xe8, 0x38, 0x78, 0xb8, 0xf8,
 0x0c, 0x4c, 0x8c, 0xcc, 0x1c, 0x5c, 0x9c, 0xdc,
 0x2c, 0x6c, 0xac, 0xec, 0x3c, 0x7c, 0xbc, 0xfc,
 0x01, 0x41, 0x81, 0xc1, 0x11, 0x51, 0x91, 0xd1,
 0x21, 0x61, 0xa1, 0xe1, 0x31, 0x71, 0xb1, 0xf1,
 0x05, 0x45, 0x85, 0xc5, 0x15, 0x55, 0x95, 0xd5,
 0x25, 0x65, 0xa5, 0xe5, 0x35, 0x75, 0xb5, 0xf5,
 0x09, 0x49, 0x89, 0xc9, 0x19, 0x59, 0x99, 0xd9,
 0x29, 0x69, 0xa9, 0xe9, 0x39, 0x79, 0xb9, 0xf9,
 0x0d, 0x4d, 0x8d, 0xcd, 0x1d, 0x5d, 0x9d, 0xdd,
 0x2d, 0x6d, 0xad, 0xed, 0x3d, 0x7d, 0xbd, 0xfd,
 0x02, 0x42, 0x82, 0xc2, 0x12, 0x52, 0x92, 0xd2,
 0x22, 0x62, 0xa2, 0xe2, 0x32, 0x72, 0xb2, 0xf2,
 0x06, 0x46, 0x86, 0xc6, 0x16, 0x56, 0x96, 0xd6,
 0x26, 0x66, 0xa6, 0xe6, 0x36, 0x76, 0xb6, 0xf6,
 0x0a, 0x4a, 0x8a, 0xca, 0x1a, 0x5a, 0x9a, 0xda,
 0x2a, 0x6a, 0xaa, 0xea, 0x3a, 0x7a, 0xba, 0xfa,
 0x0e, 0x4e, 0x8e, 0xce, 0x1e, 0x5e, 0x9e, 0xde,
 0x2e, 0x6e, 0xae, 0xee, 0x3e, 0x7e, 0xbe, 0xfe,
 0x03, 0x43, 0x83, 0xc3, 0x13, 0x53, 0x93, 0xd3,
 0x23, 0x63, 0xa3, 0xe3, 0x33, 0x73, 0xb3, 0xf3,
 0x07, 0x47, 0x87, 0xc7, 0x17, 0x57, 0x97, 0xd7,
 0x27, 0x67, 0xa7, 0xe7, 0x37, 0x77, 0xb7, 0xf7,
 0x0b, 0x4b, 0x8b, 0xcb, 0x1b, 0x5b, 0x9b, 0xdb,
 0x2b, 0x6b, 0xab, 0xeb, 0x3b, 0x7b, 0xbb, 0xfb,
 0x0f, 0x4f, 0x8f, 0xcf, 0x1f, 0x5f, 0x9f, 0xdf,
 0x2f, 0x6f, 0xaf, 0xef, 0x3f, 0x7f, 0xbf, 0xff,
};
uchar revtab2[] = {
 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70,
 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0,
 0x01, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71,
 0x81, 0x91, 0xa1, 0xb1, 0xc1, 0xd1, 0xe1, 0xf1,
 0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72,
 0x82, 0x92, 0xa2, 0xb2, 0xc2, 0xd2, 0xe2, 0xf2,
 0x03, 0x13, 0x23, 0x33, 0x43, 0x53, 0x63, 0x73,
 0x83, 0x93, 0xa3, 0xb3, 0xc3, 0xd3, 0xe3, 0xf3,
 0x04, 0x14, 0x24, 0x34, 0x44, 0x54, 0x64, 0x74,
 0x84, 0x94, 0xa4, 0xb4, 0xc4, 0xd4, 0xe4, 0xf4,
 0x05, 0x15, 0x25, 0x35, 0x45, 0x55, 0x65, 0x75,
 0x85, 0x95, 0xa5, 0xb5, 0xc5, 0xd5, 0xe5, 0xf5,
 0x06, 0x16, 0x26, 0x36, 0x46, 0x56, 0x66, 0x76,
 0x86, 0x96, 0xa6, 0xb6, 0xc6, 0xd6, 0xe6, 0xf6,
 0x07, 0x17, 0x27, 0x37, 0x47, 0x57, 0x67, 0x77,
 0x87, 0x97, 0xa7, 0xb7, 0xc7, 0xd7, 0xe7, 0xf7,
 0x08, 0x18, 0x28, 0x38, 0x48, 0x58, 0x68, 0x78,
 0x88, 0x98, 0xa8, 0xb8, 0xc8, 0xd8, 0xe8, 0xf8,
 0x09, 0x19, 0x29, 0x39, 0x49, 0x59, 0x69, 0x79,
 0x89, 0x99, 0xa9, 0xb9, 0xc9, 0xd9, 0xe9, 0xf9,
 0x0a, 0x1a, 0x2a, 0x3a, 0x4a, 0x5a, 0x6a, 0x7a,
 0x8a, 0x9a, 0xaa, 0xba, 0xca, 0xda, 0xea, 0xfa,
 0x0b, 0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b,
 0x8b, 0x9b, 0xab, 0xbb, 0xcb, 0xdb, 0xeb, 0xfb,
 0x0c, 0x1c, 0x2c, 0x3c, 0x4c, 0x5c, 0x6c, 0x7c,
 0x8c, 0x9c, 0xac, 0xbc, 0xcc, 0xdc, 0xec, 0xfc,
 0x0d, 0x1d, 0x2d, 0x3d, 0x4d, 0x5d, 0x6d, 0x7d,
 0x8d, 0x9d, 0xad, 0xbd, 0xcd, 0xdd, 0xed, 0xfd,
 0x0e, 0x1e, 0x2e, 0x3e, 0x4e, 0x5e, 0x6e, 0x7e,
 0x8e, 0x9e, 0xae, 0xbe, 0xce, 0xde, 0xee, 0xfe,
 0x0f, 0x1f, 0x2f, 0x3f, 0x4f, 0x5f, 0x6f, 0x7f,
 0x8f, 0x9f, 0xaf, 0xbf, 0xcf, 0xdf, 0xef, 0xff,
};

void
pixreverse(uchar *p, int len, int ldepth)
{
	uchar *e;
	uchar *tab;

	switch(ldepth){
	case 0:
		tab = revtab0;
		break;
	case 1:
		tab = revtab1;
		break;
	case 2:
		tab = revtab2;
		break;
	default:
		return;
	}

	for(e = p + len; p < e; p++)
		*p = tab[*p];
}
