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
#define XPERIOD	800	/* Hsync freq == 31.47 KHZ */
#define YPERIOD	525	/* Vsync freq == 59.9 HZ */
#define YBORDER 2

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
	 Cvt=		 0x06,		/*  vertical total */
	 Cvover=	 0x07,		/*  bits that didn't fit elsewhere */
	 Cmsl=		 0x09,		/*  max scan line */
	 Cvrs=		 0x10,		/*  vertical retrace start */
	 Cvre=		 0x11,		/*  vertical retrace end */
	 Cvde=	 	 0x12,		/*  vertical display end */
	 Cvbs=		 0x15,		/*  vertical blank start */
	 Cvbe=		 0x16,		/*  vertical blank end */
	 Cmode=		 0x17,		/*  mode register */
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
 *  set up like vga mode 0x12
 *	16 color (though we only use values 0x0 and 0xf)
 *	640x480
 *
 *  we assume the BIOS left the registers in a
 *  CGA-like mode.  Thus we don't set all the registers.
 */
vga12(void)
{
	int overflow;
	int msl;

	arout(Acpe, 0x00);	/* disable planes for output */

	gscreen.ldepth = 0;
	arout(Amode, 0x01);	/* color graphics mode */
	grout(Gmisc, 0x01);	/* graphics mode */
	grout(Gmode, 0x00);	/* 1 bit deep */
	grout(Grot, 0x00);	/* CPU writes bytes to video
				 * mem without modifications */

	msl = overflow = 0;
	crout(Cmode, 0xe3);	/* turn off address wrap &
				 * word mode */
	/* last scan line displayed (first is 0) */
	crout(Cvde, MAXY-1);
	overflow |= ((MAXY-1)&0x200) ? 0x40 : 0;
	overflow |= ((MAXY-1)&0x100) ? 0x2 : 0;
	/* total scan lines (including retrace) - 2 */
	crout(Cvt, (YPERIOD-2));
	overflow |= ((YPERIOD-2)&0x200) ? 0x20 : 0;
	overflow |= ((YPERIOD-2)&0x100) ? 0x1 : 0;
	/* scan lines at which vertcal retrace starts & ends */
	crout(Cvrs, (MAXY+10));
	overflow |= ((MAXY+10)&0x200) ? 0x80 : 0;
	overflow |= ((MAXY+10)&0x100) ? 0x4 : 0;
	crout(Cvre, ((YPERIOD-1)&0xf)|0xa0);	/* also disable vertical interrupts */
	/* scan lines at which vertical blanking starts & ends */
	crout(Cvbs, (MAXY+YBORDER));
	msl |= ((MAXY+YBORDER)&0x200) ? 0x20 : 0;
	overflow |= ((MAXY+YBORDER)&0x100) ? 0x8 : 0;
	crout(Cvbe, (YPERIOD-YBORDER)&0x7f);
	/* pixels per scan line (always 0 for graphics) */
	crout(Cmsl, 0x40|msl);	/* also 10th bit of line compare */
	/* the overflow bits from the other registers */
	crout(Cvover, 0x10|overflow);	/* also 9th bit of line compare */

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

	vga12();

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
