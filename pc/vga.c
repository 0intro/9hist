#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

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
	 Cmode=		 0x17,		/*  mode register */
	 Cmsl=		 0x09,		/*  max scan line */
	ARX=		0x3C0,		/* index to attribute registers */
	AR=		0x3C1,		/* attribute registers */
	 Amode=		 0x10,		/*  mode register */
	 Acpe=		 0x12,		/*  color plane enable */
};

/*
 *  screen dimensions
 */
#define MAXX	640
#define MAXY	480

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
 *  partial screen munching squares
 */
#define	DELTA	1
#define	DADDR	((long *) 0)
#define	SIDE	256
void
munch(void)
{
	ulong x,y,i,d;
	uchar *screen, tab[8], *p;

	screen = (uchar *)(0xA0000 | KZERO);
	d=0;
	tab[0] = 0x80;
	tab[1] = 0x40;
	tab[2] = 0x20;
	tab[3] = 0x10;
	tab[4] = 0x08;
	tab[5] = 0x04;
	tab[6] = 0x02;
	tab[7] = 0x01;

	for(i=0; i<MAXY*(MAXX/8); i++)
		screen[i]=0;

	for(;;){
		for(x=0; x<SIDE; x++){
			y = (x^d) % SIDE;
			p = &screen[y*(MAXX/8) + (x/8)];
			y = *p;
			*p = y ^ tab[x&7];
		}
		d+=DELTA;
	}
}

/*
 *  Set up for 4 separately addressed bit planes.  Each plane is 
 */
void
vgainit(void)
{
	uchar *display;
	int i, j, k;
	int c;

	display = (uchar *)(0xA0000 | KZERO);
	arout(Acpe, 0x0f);	/* enable all planes */
	arout(Amode, 0x01);	/* graphics mode - 4 bit pixels */
	grout(Gmisc, 0x01);	/* graphics mode */
	grout(Gmode, 0x00);	/* write mode 0, read mode 0 */
	grout(Grot, 0x00);	/* CPU writes bytes to video mem without modifications */
	crout(Cmode, 0xe3);	/* turn off address wrap & word mode */
	crout(Cmsl, 0x40);	/* 1 pixel per scan line */
	srout(Smode, 0x06);	/* extended memory, odd/even off */
	srout(Sclock, 0x01);	/* 8 bits/char */

	/*
	 *  zero out display
	 */
	srout(Smmask, 0x0f);	/* enable all 4 color planes for writing */
	for(i=0; i<MAXY*(MAXX/8); i++)
		display[i] = 0;
	
	munch();
}

