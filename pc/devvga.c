#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	<libg.h>
#include	"screen.h"
#include	"vga.h"

enum
{
	Footshift=	16,
	Footprint=	1<<Footshift,
};

/* imported */
extern	Subfont defont0;
extern Cursor curs;			/* barf */

/* exported */
Bitmap	gscreen;
Lock palettelock;			/* access to DAC registers */
Cursor curcursor;			/* current cursor */

/* vga screen */
static	Lock	screenlock;
static	ulong	colormap[Pcolours][3];

/* cga screen */
static	int	cga = 1;		/* true if in cga mode */

/* system window */
static	Rectangle window;
static	int	h, w;
static	Point	curpos;

/*
 *  screen dimensions
 */
#define	CGAWIDTH	160
#define	CGAHEIGHT	24

/*
 *  screen memory addresses
 */
#define SCREENMEM	(0xA0000 | KZERO)
#define CGASCREEN	((uchar*)(0xB8000 | KZERO))

static void nopage(int);

static Vgac vga = {
	"vga",
	nopage,

	0,
};

static Vgac *vgactlr = &vga;			/* available VGA ctlrs */
static Vgac *vgac = &vga;			/* current VGA ctlr */
static Hwgc *hwgctlr;				/* available HWGC's */
Hwgc *hwgc;					/* current HWGC */

static char interlaced[2];

/*
 *  work areas for bitblting screen characters, scrolling, and cursor redraw
 */
Bitmap chwork;
Bitmap scrollwork;
Bitmap cursorwork;

/* predefined for the stupid compiler */
static void	setscreen(int, int, int);
static void	cgascreenputc(int);
static void	cgascreenputs(char*, int);
static void	screenputc(char*);
static void	scroll(void);
static void	workinit(Bitmap*, int, int);
extern void	screenload(Rectangle, uchar*, int, int, int);
extern void	screenunload(Rectangle, uchar*, int, int, int);
static void	cursorlock(Rectangle);
static void	cursorunlock(void);

extern int	graphicssubtile(uchar*, int, int, Rectangle, Rectangle, uchar**);

/*
 *  vga device
 */
enum
{
	Qdir		= 0,
	Qvgaiob		= 1,
	Qvgaiow		= 2,
	Qvgaiol		= 3,
	Qvgactl		= 4,
	Nvga		= Qvgactl,
};
Dirtab vgadir[]={
	"vgaiob",	{ Qvgaiob },	0,	0666,
	"vgaiow",	{ Qvgaiow },	0,	0666,
	"vgaiol",	{ Qvgaiol },	0,	0666,
	"vgactl",	{ Qvgactl },	0,	0666,
};

void
vgareset(void)
{
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

static int
checkvgaport(int port, int len)
{
	if((port == 0x102 || port == 0x46E8) && len == 1)
		return 0;
	if(port >= 0x3B0 && port+len < 0x3E0)
		return 0;
	return -1;
}

long
vgaread(Chan *c, void *buf, long n, ulong offset)
{
	int port;
	uchar *cp;
	char cbuf[64];
	ushort *sp;
	ulong *lp;
	Vgac *vgacp;

	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, vgadir, Nvga, devgen);
	case Qvgactl:
		if(cga)
			return readstr(offset, buf, n, "type: cga\n");
		vgacp = vgac;
		port = sprint(cbuf, "type: %s\n", vgacp->name);
		port += sprint(cbuf+port, "size: %dx%dx%d%s\n",
			gscreen.r.max.x, gscreen.r.max.y,
			1<<gscreen.ldepth, interlaced);
		port += sprint(cbuf+port, "hwgc: ");
		if(hwgc)
			sprint(cbuf+port, "%s\n", hwgc->name);
		else
			sprint(cbuf+port, "off\n");
		return readstr(offset, buf, n, cbuf);
	case Qvgaiob:
		port = offset;
		/*if(checkvgaport(port, n))
			error(Eperm);*/
		for(cp = buf; port < offset+n; port++)
			*cp++ = vgai(port);
		return n;
	case Qvgaiow:
		if((n & 01) || (offset & 01))
			error(Ebadarg);
		n /= 2;
		for (sp = buf, port=offset; port<offset+n; port+=2)
			*sp++ = ins(port);
		return n*2;
	case Qvgaiol:
		if((n & 03) || (offset & 03))
			error(Ebadarg);
		n /= 4;
		for (lp = buf, port=offset; port<offset+n; port+=4)
			*lp++ = inl(port);
		return n*4;
	}
	error(Eperm);
	return 0;
}

Block*
vgabread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static void
vgactl(char *arg)
{
	int x, y, z;
	char *cp, *field[3];
	Hwgc *hwgcp;
	Vgac *vgacp;

	if(getfields(arg, field, 3, " ") != 2)
		error(Ebadarg);

	if(strcmp(field[0], "hwgc") == 0){
		if(strcmp(field[1], "off") == 0){
			if(hwgc){
				(*hwgc->disable)();
				hwgc = 0;
				cursoron(1);
			}
			return;
		}

		for(hwgcp = hwgctlr; hwgcp; hwgcp = hwgcp->link){
			if(strcmp(field[1], hwgcp->name) == 0){
				if(hwgc)
					(*hwgc->disable)();
				else
					cursoroff(1);
				hwgc = hwgcp;
				(*hwgc->enable)();
				setcursor(&curs);
				cursoron(1);
				return;
			}
		}
	}
	else if(strcmp(field[0], "type") == 0){
		for(vgacp = vgactlr; vgacp; vgacp = vgacp->link){
			if(strcmp(field[1], vgacp->name) == 0){
				vgac = vgacp;
				return;
			}
		}
	}
	else if(strcmp(field[0], "size") == 0){
		x = strtoul(field[1], &cp, 0);
		if(x == 0 || x > 2048)
			error(Ebadarg);

		if(*cp)
			cp++;
		y = strtoul(cp, &cp, 0);
		if(y == 0 || y > 1280)
			error(Ebadarg);

		if(*cp)
			cp++;
		switch(strtoul(cp, &cp, 0)){
		case 8:
			z = 3; break;
		case 1:
			z = 0; break;
		default:
			z = 0;
			error(Ebadarg);
		}
		interlaced[0] = *cp;

		cursoroff(1);
		setscreen(x, y, z);
		cursoron(1);
		return;
	}

	error(Ebadarg);
}

long
vgawrite(Chan *c, void *buf, long n, ulong offset)
{
	int port;
	uchar *cp;
	char cbuf[64];
	ushort *sp;
	ulong *lp;

	switch(c->qid.path&~CHDIR){
	case Qdir:
		error(Eperm);
	case Qvgactl:
		if(offset != 0 || n >= sizeof(cbuf))
			error(Ebadarg);
		memmove(cbuf, buf, n);
		cbuf[n] = 0;
		vgactl(cbuf);
		return n;
	case Qvgaiob:
		port = offset;
		/*if(checkvgaport(port, n))
			error(Eperm);*/
		for(cp = buf; port < offset+n; port++)
			vgao(port, *cp++);
		return n;
	case Qvgaiow:
		if((n & 01) || (offset & 01))
			error(Ebadarg);
		n /= 2;
		for (sp = buf, port=offset; port<offset+n; port+=2)
			outs(port, *sp++);
		return n*2;
	case Qvgaiol:
		if((n & 03) || (offset & 03))
			error(Ebadarg);
		n /= 4;
		for (lp = buf, port=offset; port<offset+n; port+=4)
			outl(port, *lp++);
		return n*4;
	}
	error(Eperm);
	return 0;
}

long
vgabwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
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

int
vgaxi(long port, uchar index)
{
	uchar data;

	switch(port){

	case Seqx:
	case Crtx:
	case Grx:
		outb(port, index);
		data = inb(port+1);
		break;

	case Attrx:
		/*
		 * Allow processor access to the colour
		 * palette registers. Writes to Attrx must
		 * be preceded by a read from Status1 to
		 * initialise the register to point to the
		 * index register and not the data register.
		 * Processor access is allowed by turning
		 * off bit 0x20.
		 */
		inb(Status1);
		if(index < 0x10){
			outb(Attrx, index);
			data = inb(Attrx+1);
			inb(Status1);
			outb(Attrx, 0x20|index);
		}
		else{
			outb(Attrx, 0x20|index);
			data = inb(Attrx+1);
		}
		break;

	default:
		return -1;
	}

	return data & 0xFF;
}

int
vgaxo(long port, uchar index, uchar data)
{
	switch(port){

	case Seqx:
	case Crtx:
	case Grx:
		/*
		 * We could use an outport here, but some chips
		 * (e.g. 86C928) have trouble with that for some
		 * registers.
		 */
		outb(port, index);
		outb(port+1, data);
		break;

	case Attrx:
		inb(Status1);
		if(index < 0x10){
			outb(Attrx, index);
			outb(Attrx, data);
			inb(Status1);
			outb(Attrx, 0x20|index);
		}
		else{
			outb(Attrx, 0x20|index);
			outb(Attrx, data);
		}
		break;

	default:
		return -1;
	}

	return 0;
}

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
	vgaxo(Crtx, 0x0A, 0xFF);		/* turn off cursor */
	memset(CGASCREEN, 0, CGAWIDTH*CGAHEIGHT);
}

/*
 *  a few well known VGA modes and the code to set them
 */
typedef struct VGAmode	VGAmode;
struct VGAmode
{
	uchar	misc;
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
	0xe7,
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
	0x63,
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
setmode(VGAmode *vga)
{
	uchar seq00, seq01;
	int i;

	/*
	 * Turn off the screen and
	 * reset the sequencer and leave it off.
	 * Load the generic VGA registers:
	 *	misc;
	 *	sequencer;
	 *	restore the sequencer reset state;
	 *	take off write-protect on crt[0x00-0x07];
	 *	crt;
	 *	graphics;
	 *	attribute;
	 * Restore the screen state.
	 */
	seq00 = vgaxi(Seqx, 0x00);
	vgaxo(Seqx, 0x00, 0x00);

	seq01 = vgaxi(Seqx, 0x01)|0x01;
	vgaxo(Seqx, 0x01, seq01|0x20);

	vgao(MiscW, vga->misc);

	for(i = 2; i < sizeof(vga->sequencer); i++)
		vgaxo(Seqx, i, vga->sequencer[i]);
	vgaxo(Seqx, 0x00, seq00);

	vgaxo(Crtx, 0x11, vga->crt[0x11] & ~0x80);
	for(i = 0; i < sizeof(vga->crt); i++)
		vgaxo(Crtx, i, vga->crt[i]);

	for(i = 0; i < sizeof(vga->graphics); i++)
		vgaxo(Grx, i, vga->graphics[i]);

	for(i = 0; i < sizeof(vga->attribute); i++)
		vgaxo(Attrx, i, vga->attribute[i]);

	vgaxo(Seqx, 0x01, seq01);
}

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
		vgac->page(i>>Footshift);
		memset(gscreen.base, 0xff, Footprint);
	}

	/* get size for a system window */
	h = defont0.height;
	w = defont0.info[' '].width;
	window.min = Pt(48, 48);
	window.max = add(window.min, Pt(10+w*80, 50*h));
	if(window.max.y >= gscreen.r.max.y)
		window.max.y = gscreen.r.max.y-1;
	if(window.max.x >= gscreen.r.max.x)
		window.max.x = gscreen.r.max.x-1;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	curpos = window.min;

	/* work areas change when dimensions change */
	workinit(&chwork, w, h);
	workinit(&scrollwork, 80*w, 1);
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
		for(i = 0; i < Pcolours; i++)
			setcolor(i, xnto32(i>>5, 3), xnto32(i>>2, 3), xnto32(i, 2));
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
		vgac->page(pg);
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
		vgac->page(pg);
		q -= Footprint;
		diff -= Footprint;
	}

	rem = e - q;

	if(rem < len){
		memmove(q, data, rem);
		pg = ++*page;
		vgac->page(pg);
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

	if(cga || !rectclip(&r, gscreen.r) || tl<=0)
		return;

	if(dolock && hwgc == 0)
		cursorlock(r);
	lock(&palettelock);

	q = byteaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);

	off = q - (uchar*)gscreen.base;
	page = off>>Footshift;
	vgac->page(page);
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

	unlock(&palettelock);
	if(dolock && hwgc == 0)
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
		vgac->page(pg);
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
		vgac->page(pg);
		q -= Footprint;
		diff -= Footprint;
	}

	rem = e - q;

	if(rem < len){
		memmove(data, q, rem);
		pg = ++*page;
		vgac->page(pg);
		q -= Footprint;
		diff -= Footprint;
		memmove(data+rem, q+rem, len-rem);
	} else
		memmove(data, q, len);

	return diff;
}

/*
 * get a tile from screen memory.
 * tile is at location r, first pixel in *data. 
 * tl is length of scan line to insert,
 * l is amount to advance data after each scan line.
 */
void
screenunload(Rectangle r, uchar *data, int tl, int l, int dolock)
{
	int y, lpart, rpart, mx, m, mr, page, sw;
	ulong off;
	uchar *q, *e;

	if(cga || !rectclip(&r, gscreen.r) || tl<=0)
		return;

	if(dolock && hwgc == 0)
		cursorlock(r);
	lock(&palettelock);

	q = byteaddr(&gscreen, r.min);
	mx = 7>>gscreen.ldepth;
	lpart = (r.min.x & mx) << gscreen.ldepth;
	rpart = (r.max.x & mx) << gscreen.ldepth;
	m = 0xFF >> lpart;
	mr = 0xFF ^ (0xFF >> rpart);

	off = q - (uchar*)gscreen.base;
	page = off>>Footshift;
	vgac->page(page);
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

	unlock(&palettelock);
	if(dolock && hwgc == 0)
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

static void
nopage(int page)
{
	USED(page);
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
		x = 0xF;
		break;
	case 3:
		x = 0xFF;
		break;
	}
	p &= x;
	p ^= x;
	lock(&palettelock);
	*pr = colormap[p][Pred];
	*pg = colormap[p][Pgreen];
	*pb = colormap[p][Pblue];
	unlock(&palettelock);
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	ulong x;

	switch(gscreen.ldepth){
	default:
		x = 0xF;
		break;
	case 3:
		x = 0xFF;
		break;
	}
	p &= x;
	p ^= x;
	lock(&palettelock);
	colormap[p][Pred] = r;
	colormap[p][Pgreen] = g;
	colormap[p][Pblue] = b;
	vgao(PaddrW, p);
	vgao(Pdata, r>>(32-6));
	vgao(Pdata, g>>(32-6));
	vgao(Pdata, b>>(32-6));
	unlock(&palettelock);
	return ~0;
}

/*
 *  software cursor
 *  and hacks for hardware cursor
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

	if(hwgc)
		(*hwgc->load)(curs);
	else for(i=0; i<16; i++){
		p = (uchar*)&set.base[i];
		*p = curs->set[2*i];
		*(p+1) = curs->set[2*i+1];
		p = (uchar*)&clr.base[i];
		*p = curs->clr[2*i];
		*(p+1) = curs->clr[2*i+1];
	}
}

int
cursoron(int dolock)
{
	int xoff, yoff, s, ret;
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
		return 0;
	if(dolock){
		s = 0;		/* to avoid compiler warning */
		lock(&cursor);
	} else
		s = spllo();	/* to avoid freezing out the eia ports */

	ret = 0;
	if(hwgc)
		ret = (*hwgc->move)(mousexy());
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
	else
		splx(s);

	return ret;
}

void
cursoroff(int dolock)
{
	if(hwgc)
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

void
addhwgclink(Hwgc *hwgcp)
{
	hwgcp->link = hwgctlr;
	hwgctlr = hwgcp;
}

void
addvgaclink(Vgac *vgacp)
{
	vgacp->link = vgactlr;
	vgactlr = vgacp;
}
