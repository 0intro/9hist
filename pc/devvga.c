#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	<libg.h>
#include	"screen.h"

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

enum
{
	Footshift=	16,
	Footprint=	1<<Footshift,
};

/* imported */
extern	Subfont defont0;

/* exported */
Bitmap	gscreen;

/* vga screen */
static	Lock	screenlock;
static	Lock	loadlock;
static	ulong	colormap[256][3];

/* cga screen */
static	int	cga = 1;		/* true if in cga mode */

/* system window */
static	Rectangle window;
static	int	h, w;
static	Point	curpos;

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
 *  definitions of known cards
 */
typedef struct Vgacard	Vgacard;
struct Vgacard
{
	char	*name;
	void	(*setpage)(int);	/* routine to page though display memory */
	void	(*mvcursor)(Point);	/* routine to move hardware cursor */
};

enum
{
	Ati,		/* ATI */
	Pvga1a,		/* paradise */
	Trident,	/* Trident 8900 */
	Tseng,		/* tseng labs te4000 */
	Cirrus,		/* Cirrus CLGD542X */
	S3,
	Generic,
};

static void	nopage(int), tsengpage(int), tridentpage(int), parapage(int);
static void	atipage(int), cirruspage(int), s3page(int);

static void	nomvcursor(Point);

Vgacard vgachips[] =
{
[Ati]		{ "ati", atipage, nomvcursor, },
[Pvga1a]	{ "pvga1a", parapage, nomvcursor, },
[Trident]	{ "trident", tridentpage, nomvcursor, },
[Tseng]		{ "tseng", tsengpage, nomvcursor, },
[Cirrus]	{ "cirrus", cirruspage, nomvcursor, },
[S3]		{ "s3", s3page, nomvcursor, },
[Generic]	{ "generic", nopage, nomvcursor, },
		{ 0, 0, },
};

Vgacard	*vgacard;	/* current vga card */
int hwcursor;

/*
 *  work areas for bitblting screen characters, scrolling, and cursor redraw
 */
Bitmap chwork;
Bitmap scrollwork;
Bitmap cursorwork;

/* predefined for the stupid compiler */
static void	setscreen(int, int, int);
static uchar	srin(int);
static void	genout(int, int);
static void	srout(int, int);
static void	grout(int, int);
static void	arout(int, int);
static void	crout(int, int);
static void	cgascreenputc(int);
static void	cgascreenputs(char*, int);
static void	screenputc(char*);
static void	scroll(void);
static ulong	xnto32(uchar, int);
static void	workinit(Bitmap*, int, int);
extern void	screenload(Rectangle, uchar*, int, int, int);
extern void	screenunload(Rectangle, uchar*, int, int, int);
static void	cursorlock(Rectangle);
static void	cursorunlock(void);

extern int	graphicssubtile(uchar*, int, int, Rectangle, Rectangle, uchar**);


/*
 *  start the screen in CGA mode.  Create the fonts for VGA.  Called by
 *  main().
 */
void
screeninit(void)
{
	int i;
	ulong *l;

	/*
	 *  swizzle the font longs.
	 */
	l = defont0.bits->base;
	for(i = defont0.bits->width*Dy(defont0.bits->r); i > 0; i--, l++)
		*l = (*l<<24) | ((*l>>8)&0x0000ff00) | ((*l<<8)&0x00ff0000) | (*l>>24);

	/*
	 *  start in CGA mode
	 */
	cga = 1;
	crout(0x0a, 0xff);		/* turn off cursor */
	memset(CGASCREEN, 0, CGAWIDTH*CGAHEIGHT);
}

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
	Qvgactl=	5,
	Nvga=		5,
};
Dirtab vgadir[]={
	"vgasize",	{Qvgasize},	0,		0666,
	"vgatype",	{Qvgatype},	0,		0666,
	"vgaport",	{Qvgaport},	0,		0666,
	"vgaportw",	{Qvgaportw},	0,		0666,
	"vgactl",	{Qvgactl},	0,		0666,
};

void
vgareset(void)
{
	vgacard = &vgachips[Generic];
	cursor.disable++;
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
	case Qvgactl:
		if(offset != 0 || n >= sizeof(cbuf))
			error(Ebadarg);
		memmove(cbuf, buf, n);
		cbuf[n] = 0;
		if(strncmp(cbuf, "hwcursor", 8) == 0)
			hwcursor = 1;
		break;
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
		|| maxx > 1600 || maxy > 1280
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
 *  accessing card registers
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

/*
 *  a few well known VGA modes and the code to set them
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

/*
 *  reconfigure screen shape
 */
static void
setscreen(int maxx, int maxy, int ldepth)
{
	int i, x, l, tl;
	uchar *a;
	Rectangle r;

	if(waserror()){
		unlock(&screenlock);
		nexterror();
	}
	lock(&screenlock);

	/* set default mode, a user program sets more complicated ones */
	switch(ldepth){
	case 0:
		setmode(&mode12);
		break;
	case 3:
		setmode(&mode13);
		break;
	default:
		error(Ebadarg);
	}

	/* setup a bitmap for the new size */
	gscreen.ldepth = ldepth;
	gscreen.width = (maxx*(1<<gscreen.ldepth)+31)/32;
	gscreen.base = (void*)SCREENMEM;
	gscreen.r.min = Pt(0, 0);
	gscreen.r.max = Pt(maxx, maxy);
	gscreen.clipr = gscreen.r;
	for(i = 0; i < gscreen.width*BY2WD*maxy; i += Footprint){
		vgacard->setpage(i>>Footshift);
		memset(gscreen.base, 0xff, Footprint);
	}

	/* get size for a system window */
	h = defont0.height;
	w = defont0.info[' '].width;
	window.min = Pt(48, 48);
	window.max = add(window.min, Pt(10+w*64, 36*h));
	if(window.max.y >= gscreen.r.max.y)
		window.max.y = gscreen.r.max.y-1;
	if(window.max.x >= gscreen.r.max.x)
		window.max.x = gscreen.r.max.x-1;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	curpos = window.min;

	/* work areas change when dimensions change */
	workinit(&chwork, w, h);
	workinit(&scrollwork, 64*w, 1);
	workinit(&scrollwork, Dx(window), 1);
	cursorinit();

	/* clear the system window */
	l = scrollwork.width * BY2WD;
	memset(scrollwork.base, 0, l);
	tl = graphicssubtile(0, l, gscreen.ldepth, gscreen.r, window, &a);
	for(i = window.min.y; i < window.max.y; i++){
		r = Rect(window.min.x, i, window.max.x, i + 1);
		screenload(r, (uchar*)scrollwork.base, tl, l, 1);
	}

	unlock(&screenlock);
	poperror();

	/* default color map (has to be outside the lock) */
	switch(ldepth){
	case 3:
		for(i = 0; i < 256; i++)
			setcolor(i, xnto32(i>>5, 3), xnto32(i>>2, 3), xnto32(i<<1, 2));
		setcolor(0x55, xnto32(0x15, 6), xnto32(0x15, 6), xnto32(0x15, 6));
		setcolor(0xaa, xnto32(0x2a, 6), xnto32(0x2a, 6), xnto32(0x2a, 6));
		setcolor(0xff, xnto32(0x3f, 6), xnto32(0x3f, 6), xnto32(0x3f, 6));
		break;
	case 2:
	case 1:
	case 0:
		for(i = 0; i < 16; i++){
			x = xnto32((i*63)/15, 6);
			setcolor(i, x, x, x);
		}
		break;
	}

	/* switch software to graphics mode */
	cga = 0;
}

/*
 *  init a bitblt work area
 */
static void
workinit(Bitmap *bm, int maxx, int maxy)
{
	bm->ldepth = gscreen.ldepth;
	bm->r = Rect(0, 0, maxx, maxy);
	if(gscreen.ldepth != 3)
		bm->r.max.x += 1<<(3-gscreen.ldepth);
	bm->clipr = bm->r;
	bm->width = ((bm->r.max.x << gscreen.ldepth) + 31) >> 5;
	if(bm->base == 0)
		bm->base = xalloc(maxx*maxy);
}

/*
 *  Load a byte into screen memory.  Assume that if the page
 *  is wrong we just need to increment it.
 */
static int
byteload(uchar *q, uchar *data, int m, int *page, uchar *e)
{
	int pg;
	int diff;

	diff = 0;
	if(q >= e){
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
	}
	*q ^= (*data^*q) & m;
	return diff;
}

/*
 *  Load adjacent bytes into a screen memory.  Assume that if the page
 *  is wrong we just need to increment it.
 */
static int
lineload(uchar *q, uchar *data, int len, int *page, uchar *e)
{
	int rem, pg;
	int diff;

	diff = 0;
	if(q >= e){
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
	}

	rem = e - q;

	if(rem < len){
		memmove(q, data, rem);
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
		memmove(q+rem, data+rem, len-rem);
	} else
		memmove(q, data, len);

	return diff;
}

/*
 *   paste tile into hard screen.
 *   tile is at location r, first pixel in *data.  tl is length of scan line to insert,
 *   l is amount to advance data after each scan line.
 */
void
screenload(Rectangle r, uchar *data, int tl, int l, int dolock)
{
	int y, lpart, rpart, mx, m, mr, page, sw;
	ulong off;
	uchar *q, *e;

	if(!rectclip(&r, gscreen.r) || tl<=0)
		return;

	if(dolock && hwcursor == 0)
		cursorlock(r);
	lock(&loadlock);

	q = byteaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);

	off = q - (uchar*)gscreen.base;
	page = off>>Footshift;
	vgacard->setpage(page);
	q = ((uchar*)gscreen.base) + (off&(Footprint-1));

	sw = gscreen.width*sizeof(ulong);
	e = ((uchar*)gscreen.base) + Footprint;

	/* may need to do bit insertion on edges */
	if(tl <= 0){
		;
	}else if(tl == 1){	/* all in one byte */
		if(rpart)
			m &= mr;
		for(y=r.min.y; y<r.max.y; y++){
			if(q < e)
				*q ^= (*data^*q) & m;
			else
				q += byteload(q, data, m, &page, e);
			q += sw;
			data += l;
		}
	}else if(lpart==0 && rpart==0){	/* easy case */
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e)
				memmove(q, data, tl);
			else
				q += lineload(q, data, tl, &page, e);
			q += sw;
			data += l;
		}
	}else if(rpart==0){
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				*q ^= (*data^*q) & m;
				memmove(q+1, data+1, tl-1);
			} else {
				q += byteload(q, data, m, &page, e);
				q += lineload(q+1, data+1, tl-1, &page, e);
			}
			q += sw;
			data += l;
		}
	}else if(lpart == 0){
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				memmove(q, data, tl-1);
				q[tl-1] ^= (data[tl-1]^q[tl-1]) & mr;
			} else {	/* new page */
				q += lineload(q, data, tl-1, &page, e);
				q += byteload(q+tl-1, data+tl-1, mr, &page, e);
			}
			q += sw;
			data += l;
		}
	}else for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				*q ^= (*data^*q) & m;
				if(tl > 2)
					memmove(q+1, data+1, tl-2);
				q[tl-1] ^= (data[tl-1]^q[tl-1]) & mr;
			} else {	/* new page */
				q += byteload(q, data, m, &page, e);
				if(tl > 2)
					q += lineload(q+1, data+1, tl-2, &page, e);
				q += byteload(q+tl-1, data+tl-1, mr, &page, e);
			}
			q += sw;
			data += l;
		}

	unlock(&loadlock);
	if(dolock && hwcursor == 0)
		cursorunlock();
}

/*
 *  Get a byte from screen memory.  Assume that if the page
 *  is wrong we just need to increment it.
 */
static int
byteunload(uchar *q, uchar *data, int m, int *page, uchar *e)
{
	int pg;
	int diff;

	diff = 0;
	if(q >= e){
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
	}
	*data ^= (*q^*data) & m;
	return diff;
}

/*
 *  Get a vector of bytes from screen memory.  Assume that if the page
 *  is wrong we just need to increment it.
 */
static int
lineunload(uchar *q, uchar *data, int len, int *page, uchar *e)
{
	int rem, pg;
	int diff;

	diff = 0;
	if(q >= e){
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
	}

	rem = e - q;

	if(rem < len){
		memmove(data, q, rem);
		pg = ++*page;
		vgacard->setpage(pg);
		q -= Footprint;
		diff -= Footprint;
		memmove(data+rem, q+rem, len-rem);
	} else
		memmove(data, q, len);

	return diff;
}

/*
 *   paste tile into hard screen.
 *   tile is at location r, first pixel in *data.  tl is length of scan line to insert,
 *   l is amount to advance data after each scan line.
 */
void
screenunload(Rectangle r, uchar *data, int tl, int l, int dolock)
{
	int y, lpart, rpart, mx, m, mr, page, sw;
	ulong off;
	uchar *q, *e;

	if(!rectclip(&r, gscreen.r) || tl<=0)
		return;

	if(dolock && hwcursor == 0)
		cursorlock(r);
	lock(&loadlock);

	q = byteaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);

	off = q - (uchar*)gscreen.base;
	page = off>>Footshift;
	vgacard->setpage(page);
	q = ((uchar*)gscreen.base) + (off&(Footprint-1));

	sw = gscreen.width*sizeof(ulong);
	e = ((uchar*)gscreen.base) + Footprint;

	/* may need to do bit insertion on edges */
	if(tl <= 0){
		;
	}else if(tl == 1){	/* all in one byte */
		if(rpart)
			m &= mr;
		for(y=r.min.y; y<r.max.y; y++){
			if(q < e)
				*data ^= (*q^*data) & m;
			else
				q += byteunload(q, data, m, &page, e);
			q += sw;
			data += l;
		}
	}else if(lpart==0 && rpart==0){	/* easy case */
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e)
				memmove(data, q, tl);
			else
				q += lineunload(q, data, tl, &page, e);
			q += sw;
			data += l;
		}
	}else if(rpart==0){
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				*data ^= (*q^*data) & m;
				memmove(data+1, q+1, tl-1);
			} else {
				q += byteunload(q, data, m, &page, e);
				q += lineunload(q+1, data+1, tl-1, &page, e);
			}
			q += sw;
			data += l;
		}
	}else if(lpart == 0){
		for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				memmove(data, q, tl-1);
				data[tl-1] ^= (q[tl-1]^data[tl-1]) & mr;
			} else {	/* new page */
				q += lineunload(q, data, tl-1, &page, e);
				q += byteunload(q+tl-1, data+tl-1, mr, &page, e);
			}
			q += sw;
			data += l;
		}
	}else for(y=r.min.y; y<r.max.y; y++){
			if(q + tl <= e){
				*data ^= (*q^*data) & m;
				if(tl > 2)
					memmove(data+1, q+1, tl-2);
				data[tl-1] ^= (q[tl-1]^data[tl-1]) & mr;
			} else {	/* new page */
				q += byteunload(q, data, m, &page, e);
				if(tl > 2)
					q += lineunload(q+1, data+1, tl-2, &page, e);
				q += byteunload(q+tl-1, data+tl-1, mr, &page, e);
			}
			q += sw;
			data += l;
		}

	unlock(&loadlock);
	if(dolock && hwcursor == 0)
		cursorunlock();
}

/*
 *  write a string to the screen
 */
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

	unlock(&screenlock);
}


/*
 *  expand n bits of color to 32
 */
static ulong
xnto32(uchar x, int n)
{
	int s;
	ulong y;

	x &= (1<<n)-1;
	y = 0;
	for(s = 32 - n; s > 0; s -= n)
		y |= x<<s;
	if(s < 0)
		y |= x>>(-s);
	return y;
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
static void
s3page(int page)
{
	uchar crt51;

	/*
	 * I don't understand why these are different.
	 */
	if(gscreen.ldepth == 3){
		/*
		 * The S3 registers need to be unlocked for this.
		 * Let's hope they are already:
		 *	crout(0x38, 0x48);
		 *	crout(0x39, 0xA0);
		 *
		 * The page is 6 bits, the lower 4 bits in Crt35<3:0>,
		 * the upper 2 in Crt51<3:2>.
		 */
		crout(0x35, page & 0x0F);
		outb(CRX, 0x51);
		crt51 = (0xF3 & inb(CR))|((page & 0x30)>>2);
		outb(CR, crt51);
	}
	else
		crout(0x35, (page<<2) & 0x0C);
}

/*
 *  hardware cursor routines
 */
static void
nomvcursor(Point p)
{
	USED(p.x);
}

/*
 *  character mode console
 */
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

/*
 *  graphics mode console
 */
#define LINE2SCROLL 4
static void
scroll(void)
{
	int from, tl, l, diff;
	uchar *a;
	Rectangle r;

	diff = h*LINE2SCROLL;
	l = scrollwork.width * BY2WD;
	tl = graphicssubtile(0, l, gscreen.ldepth, gscreen.r, window, &a);

	/* move lines up */
	for(from = window.min.y + diff; from < window.max.y; from++){
		r = Rect(window.min.x, from, window.max.x, from + 1);
		screenunload(r, (uchar*)scrollwork.base, tl, l, 1);
		r = Rect(window.min.x, from - diff, window.max.x, from - diff + 1);
		screenload(r, (uchar*)scrollwork.base, tl, l, 1);
	}

	/* clear bottom */
	memset(scrollwork.base, 0, l);
	for(from = window.max.y - diff; from < window.max.y; from++){
		r = Rect(window.min.x, from, window.max.x, from + 1);
		screenload(r, (uchar*)scrollwork.base, tl, l, 1);
	}
	
	curpos.y -= diff;
}

static void
screenputc(char *buf)
{
	int pos, l, tl, off;
	uchar *a;
	Rectangle r;

	switch(buf[0]) {
	case '\n':
		if(curpos.y+h >= window.max.y)
			scroll();
		curpos.y += h;
		screenputc("\r");
		break;
	case '\r':
		curpos.x = window.min.x;
		break;
	case '\t':
		pos = (curpos.x-window.min.x)/w;
		pos = 8-(pos%8);
		curpos.x += pos*w;
		break;
	case '\b':
		if(curpos.x-w >= window.min.x){
			curpos.x -= w;
			screenputc(" ");
			curpos.x -= w;
		}
		break;
	default:
		if(curpos.x >= window.max.x-w)
			screenputc("\n");

		/* tile width */
		r.min = curpos;
		r.max = add(r.min, Pt(w, h));
		off = ((1<<gscreen.ldepth)*r.min.x) & 7;
		l = chwork.width*BY2WD;
		tl = graphicssubtile(0, l, gscreen.ldepth, gscreen.r, r, &a);

		/* add char into work area */
		subfstring(&chwork, Pt(off, 0), &defont0, buf, S);

		/* move work area to screen */
		screenload(r, (uchar*)chwork.base, tl, l, 0);

		curpos.x += w;
	}
}

int
screenbits(void)
{
	return 1<<gscreen.ldepth;	/* bits per pixel */
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	ulong x;

	switch(gscreen.ldepth){
	default:
		x = 0xf;
		break;
	case 3:
		x = 0xff;
		break;
	}
	p &= x;
	p ^= x;
	lock(&screenlock);
	*pr = colormap[p][0];
	*pg = colormap[p][1];
	*pb = colormap[p][2];
	unlock(&screenlock);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	ulong x;

	switch(gscreen.ldepth){
	default:
		x = 0xf;
		break;
	case 3:
		x = 0xff;
		break;
	}
	p &= x;
	p ^= x;
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
 *  software cursor
 */

/*
 *  area to store the bits that are behind the cursor
 */
static ulong backbits[16*4];
static ulong clrbits[16];
static ulong setbits[16];

/*
 *  the white border around the cursor
 */
Bitmap	clr =
{
	{0, 0, 16, 16},
	{0, 0, 16, 16},
	0,
	clrbits,
	0,
	1,
};

/*
 *  the black center of the cursor
 */
Bitmap	set =
{
	{0, 0, 16, 16},
	{0, 0, 16, 16},
	0,
	setbits,
	0,
	1,
};

void
cursorinit(void)
{
	static int already;

	lock(&cursor);

	workinit(&cursorwork, 16, 16);
	cursor.l = cursorwork.width*BY2WD;

	if(!already){
		cursor.disable--;
		already = 1;
	}

	unlock(&cursor);
}

void
setcursor(Cursor *curs)
{
	uchar *p;
	int i;

	for(i=0; i<16; i++){
		p = (uchar*)&set.base[i];
		*p = curs->set[2*i];
		*(p+1) = curs->set[2*i+1];
		p = (uchar*)&clr.base[i];
		*p = curs->clr[2*i];
		*(p+1) = curs->clr[2*i+1];
	}
}

void
cursoron(int dolock)
{
	int xoff, yoff;
	Rectangle r;
	uchar *a;
	struct {
		Bitmap *dm;
		Point p;
		Bitmap *sm;
		Rectangle r;
		Fcode f;
	} xx;

	if(cursor.disable)
		return;
	if(dolock)
		lock(&cursor);

	if(hwcursor)
		(*vgacard->mvcursor)(mousexy());
	else if(cursor.visible++ == 0){
		cursor.r.min = mousexy();
		cursor.r.max = add(cursor.r.min, Pt(16, 16));
		cursor.r = raddp(cursor.r, cursor.offset);
	
		/* offsets into backup area and clr/set bitmaps */
		r.min = Pt(0, 0);
		if(cursor.r.min.x < 0){
			xoff = cursor.r.min.x;
			r.min.x = -xoff;
		} else
			xoff = ((1<<gscreen.ldepth)*cursor.r.min.x) & 7;
		if(cursor.r.min.y < 0){
			yoff = cursor.r.min.y;
			r.min.y = -yoff;
		} else
			yoff = 0;
		r.max = add(r.min, Pt(16, 16));
	
		/* clip the cursor rectangle */
		xx.dm = &cursorwork;
		xx.p = Pt(xoff, yoff);
		xx.sm = &gscreen;
		xx.r = cursor.r;
		bitbltclip(&xx);
	
		/* tile width */
		cursor.tl = graphicssubtile(0, cursor.l, gscreen.ldepth,
				gscreen.r, xx.r, &a);
		if(cursor.tl > 0){
			/* get tile */
			screenunload(xx.r, (uchar*)cursorwork.base, cursor.tl, cursor.l, 0);
	
			/* save for cursoroff */
			memmove(backbits, cursorwork.base, cursor.l*16);
	
			/* add mouse into work area */
			bitblt(&cursorwork, xx.p, &clr, r, D&~S);
			bitblt(&cursorwork, xx.p, &set, r, S|D);
	
			/* put back tile */
			cursor.clipr = xx.r;
			screenload(xx.r, (uchar*)cursorwork.base, cursor.tl, cursor.l, 0);
		}
	}

	if(dolock)
		unlock(&cursor);
}

void
cursoroff(int dolock)
{
	if(hwcursor)
		return;
	if(cursor.disable)
		return;
	if(dolock)
		lock(&cursor);

	if(--cursor.visible == 0 && cursor.tl > 0)
		screenload(cursor.clipr, (uchar*)backbits, cursor.tl, cursor.l, 0);

	if(dolock)
		unlock(&cursor);
}

static void
cursorlock(Rectangle r)
{
	lock(&cursor);
	if(rectXrect(cursor.r, r)){
		cursoroff(0);
		cursor.frozen = 1;
	}
	cursor.disable++;
	unlock(&cursor);
}

static void
cursorunlock(void)
{
	lock(&cursor);
	cursor.disable--;
	if(cursor.frozen)
		cursoron(0);
	cursor.frozen = 0;
	unlock(&cursor);
}
