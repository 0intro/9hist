#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	<libg.h>
#include	<gnot.h>
#include	"screen.h"

#define	MINX	8

extern	GFont	defont0;
GFont		*defont;

struct{
	Point	pos;
	int	bwid;
}out;

/*
 *  screen dimensions
 */
#define MAXX	640
#define MAXY	480

#define SCREENMEM	(0xA0000 | KZERO)

GBitmap	gscreen =
{
	(ulong*)SCREENMEM,
	0,
	640/32,
	0,
	0, 0, MAXX, MAXY,
	0
};

enum
{
	GRX=		0x3CE,		/* index to graphics registers */
	GR=		0x3CF,		/* graphics registers */
	 Grot=		 0x03,		/*  data rotate register */
	 Gmode=		 0x05,		/*  mode register */
	 Gmisc=		 0x06,		/*  miscillaneous register */
	 Grms=		 0x04,		/*  read map select register */
	SRX=		0x3C4,		/* index to sequence registers */
	SR=		0x3C5,		/* sequence registers */
	 Sclock=	 0x01,		/*  clocking register */
	 Smode=		 0x04,		/*  mode register */
	 Smmask=	 0x02,		/*  map mask */
	CRX=		0x3D4,		/* index to crt registers */
	CR=		0x3D5,		/* crt registers */
	 Cvertend=	 0x12,		/*  vertical display end */
	 Cmode=		 0x17,		/*  mode register */
	 Cmsl=		 0x09,		/*  max scan line */
	ARX=		0x3C0,		/* index to attribute registers */
	AR=		0x3C1,		/* attribute registers */
	 Amode=		 0x10,		/*  mode register */
	 Acpe=		 0x12,		/*  color plane enable */
};

/*
 *  routines for setting vga registers
 */
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
	outb(ARX, reg | 0x20);
	outb(AR, val);
}
void
crout(int reg, int val)
{
	outb(CRX, reg);
	outb(CR, val);
}

/*
 *  m is a bit mask of planes to be affected by CPU writes
 */
vgawrmask(int m)
{
	srout(Smmask, m&0xf);
}

/*
 *  p is the plane that will respond to CPU reads
 */
vgardplane(int p)
{
	grout(Grms, p&3);
}

/*
 *  2 bit deep display.  the bits are adjacent.  maybe this
 *  will work
 *	4 color
 *	640x480
 */
vga2(void)
{
	int i;
	arout(Acpe, 0x00);	/* disable planes for output */

	gscreen.ldepth = 1;
	arout(Amode, 0x01);	/* color graphics mode */
	grout(Gmisc, 0x01);	/* graphics mode */
	grout(Gmode, 0x30);	/* 2 bits deep, even bytes are
				 * planes 0 and 2, odd are planes
				 * 1 and 3 */
	grout(Grot, 0x00);	/* CPU writes bytes to video
				 * mem without modifications */
	crout(Cmode, 0xe3);	/* turn off address wrap &
				 * word mode */
	crout(Cmsl, 0x40);	/* 1 pixel per scan line */
	crout(Cvertend, MAXY);	/* 480 lne display */
	srout(Smode, 0x06);	/* extended memory, odd/even */
	srout(Sclock, 0x01);	/* 8 bits/char */
	srout(Smmask, 0x0f);	/* enable 2 planes for writing */

	arout(Acpe, 0x0f);	/* enable 2 planes for output */
	for(i = 0; i < 128*1024;){
		((uchar*)SCREENMEM)[i++] = 0x1b;
		((uchar*)SCREENMEM)[i++] = 0xe4;
	}
	for(;;);
}

/*
 *  set up like vga mode 0x11
 *	2 color
 *	640x480
 */
vga1(void)
{
	arout(Acpe, 0x00);	/* disable planes for output */

	gscreen.ldepth = 0;
	arout(Amode, 0x01);	/* color graphics mode */
	grout(Gmisc, 0x01);	/* graphics mode */
	grout(Gmode, 0x00);	/* 1 bit deep */
	grout(Grot, 0x00);	/* CPU writes bytes to video
				 * mem without modifications */
	crout(Cmode, 0xe3);	/* turn off address wrap &
				 * word mode */
	crout(Cmsl, 0x40);	/* 1 pixel per scan line */
	crout(Cvertend, MAXY-1);	/* 480 lne display */
	srout(Smode, 0x06);	/* extended memory,
				 * odd/even off */
	srout(Sclock, 0x01);	/* 8 bits/char */
	srout(Smmask, 0x0f);	/* enable 4 planes for writing */

	arout(Acpe, 0x0f);	/* enable 4 planes for output */
}

void
screeninit(void)
{
	int i, j, k;
	int c;
	ulong *l;

	vga1();

	/*
	 *  swizzle the font longs.
	 *  we do it here since the font is initialized with big
	 *  endian longs.
	 */
	defont = &defont0;
	l = defont->bits->base;
	for(i = defont->bits->width*Dy(defont->bits->r); i > 0; i--, l++)
		*l = (*l<<24) | ((*l>>8)&0x0000ff00) | ((*l<<8)&0x00ff0000) | (*l>>24);

	gbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, flipD[0]);
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;
}

void
screenputc(int c)
{
	char buf[2];
	int nx;

	if(c == '\n'){
		out.pos.x = MINX;
		out.pos.y += defont0.height;
		if(out.pos.y > gscreen.r.max.y-defont0.height)
			out.pos.y = gscreen.r.min.y;
		gbitblt(&gscreen, Pt(0, out.pos.y), &gscreen,
		    Rect(0, out.pos.y, gscreen.r.max.x, out.pos.y+2*defont0.height), flipD[0]);
	}else if(c == '\t'){
		out.pos.x += (8-((out.pos.x-MINX)/out.bwid&7))*out.bwid;
		if(out.pos.x >= gscreen.r.max.x)
			screenputc('\n');
	}else if(c == '\b'){
		if(out.pos.x >= out.bwid+MINX){
			out.pos.x -= out.bwid;
			screenputc(' ');
			out.pos.x -= out.bwid;
		}
	}else{
		if(out.pos.x >= gscreen.r.max.x-out.bwid)
			screenputc('\n');
		buf[0] = c&0x7F;
		buf[1] = 0;
		out.pos = gstring(&gscreen, out.pos, defont, buf, flipD[S]);
	}
}

void
screenputs(char *s, int n)
{
	while(n-- > 0)
		screenputc(*s++);
}

int
screenbits(void)
{
	return 1;	/* bits per pixel */
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
	return 0;	/* can't change mono screen colormap */
}

int
hwcursset(uchar *s, uchar *c, int ox, int oy)
{
	return 0;
}

int
hwcursmove(int x, int y)
{
	return 0;
}

void
mouseclock(void)
{
	mouseupdate(1);
}

vgaset(char *cmd)
{
	int set;
	int reg;
	int val;

	set = *cmd++;
	cmd++;
	reg = strtoul(cmd, &cmd, 0);
	cmd++;
	val = strtoul(cmd, &cmd, 0);
	switch(set){
	case 'a':
		arout(reg, val);
		break;
	case 'g':
		grout(reg, val);
		break;
	case 'c':
		crout(reg, val);
		break;
	case 's':
		srout(reg, val);
		break;
	}
}
