#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"errno.h"

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

GBitmap	gscreen =
{
	(ulong*)((4*1024*1024-256*1024)|KZERO),	/* BUG */
	0,
	64,
	0,
	0, 0, 1024, 1024,
	0
};

void
screeninit(void)
{
	/*
	 * Read HEX switch to set ldepth
	 */
	if(*(uchar*)MOUSE & (1<<4))
		gscreen.ldepth = 1;
	defont = &defont0;	/* save space; let bitblt do the conversion work */
	kbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, 0);
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
		kbitblt(&gscreen, Pt(0, out.pos.y), &gscreen,
		    Rect(0, out.pos.y, gscreen.r.max.x, out.pos.y+2*defont0.height), 0);
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
		out.pos = gbitbltstring(&gscreen, out.pos, defont, buf, S);
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
	if(*(uchar*)MOUSE & (1<<4))
		return 2;
	else
		return 1;
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
mouseclock(void)	/* called spl6 */
{
	++mouse.clock;
}

void
mousetry(Ureg *ur)
{
	int s;

	if(mouse.clock && mouse.track && (ur->sr&SPL(7)) == 0 && canlock(&cursor)){
		s = spl1();
		mouseupdate(0);
		splx(s);
		unlock(&cursor);
		wakeup(&mouse.r);
	}
}
