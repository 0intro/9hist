#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

static ulong backbits = (Backgnd<<24)|(Backgnd<<16)|(Backgnd<<8)|Backgnd;
static Memdata backdata = {
	nil,
	&backbits
};
static Memimage xback = {
	{ 0, 0, 1, 1 },
	{ -100000, -100000, 100000, 100000 },
	3,
	1,
	&backdata,
	0,
	1
};
static Memimage* back = &xback;

static ulong consbits = 0;
static Memdata consdata = {
	nil,
	&consbits
};
static Memimage conscol = {
	{ 0, 0, 1, 1 },
	{ -100000, -100000, 100000, 100000 },
	3,
	1,
	&consdata,
	0,
	1
};

static Point curpos;
static Rectangle window;
static int *xp;
static int xbuf[256];
static Lock vgascreenlock;

static void
vgascroll(VGAscr* scr)
{
	int h, o;
	Point p;
	Rectangle r;

	h = scr->memdefont->height;
	o = 8*h;
	r = Rpt(window.min, Pt(window.max.x, window.max.y-o));
	p = Pt(window.min.x, window.min.y+o);
	memimagedraw(scr->gscreen, r, scr->gscreen, p, memones, p);
	r = Rpt(Pt(window.min.x, window.max.y-o), window.max);
	memimagedraw(scr->gscreen, r, back, ZP, memones, ZP);

	curpos.y -= o;
}

static void
vgascreenputc(VGAscr* scr, char* buf, Rectangle *flushr)
{
	Point p;
	int h, w, pos;
	Rectangle r;

	if(xp < xbuf || xp >= &xbuf[sizeof(xbuf)])
		xp = xbuf;

	h = scr->memdefont->height;
	switch(buf[0]){

	case '\n':
		if(curpos.y+h >= window.max.y){
			vgascroll(scr);
			*flushr = window;
		}
		curpos.y += h;
		vgascreenputc(scr, "\r", flushr);
		break;

	case '\r':
		xp = xbuf;
		curpos.x = window.min.x;
		break;

	case '\t':
		p = memsubfontwidth(scr->memdefont, " ");
		w = p.x;
		*xp++ = curpos.x;
		pos = (curpos.x-window.min.x)/w;
		pos = 4-(pos%4);
		r = Rect(curpos.x, curpos.y, curpos.x+pos*w, curpos.y+h);
		memimagedraw(scr->gscreen, r, back, back->r.min, memones, back->r.min);
		bbox(flushr, r);
		curpos.x += pos*w;
		break;

	case '\b':
		if(xp <= xbuf)
			break;
		xp--;
		r = Rect(*xp, curpos.y, curpos.x, curpos.y+h);
		memimagedraw(scr->gscreen, r, back, back->r.min, memones, back->r.min);
		bbox(flushr, r);
		curpos.x = *xp;
		break;

	default:
		p = memsubfontwidth(scr->memdefont, buf);
		w = p.x;

		if(curpos.x >= window.max.x-w)
			vgascreenputc(scr, "\n", flushr);

		*xp++ = curpos.x;
		r = Rect(curpos.x, curpos.y, curpos.x+w, curpos.y+h);
		memimagedraw(scr->gscreen, r, back, back->r.min, memones, back->r.min);
		memimagestring(scr->gscreen, curpos, &conscol, scr->memdefont, buf);
		bbox(flushr, r);
		curpos.x += w;
	}
}

static void
vgascreenputs(char* s, int n)
{
	int i;
	Rune r;
	char buf[4];
	VGAscr *scr;
	Rectangle flushr;

	scr = &vgascreen[0];

	if(!islo()){
		/*
		 * Don't deadlock trying to
		 * print in an interrupt.
		 */
		if(!canlock(&vgascreenlock))
			return;
	}
	else
		lock(&vgascreenlock);

	flushr = Rect(10000, 10000, -10000, -10000);

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
		vgascreenputc(scr, buf, &flushr);
	}
	flushmemscreen(flushr);

	unlock(&vgascreenlock);
}

void
vgascreenwin(VGAscr* scr)
{
	int h, w;

	h = scr->memdefont->height;
	w = scr->memdefont->info[' '].width;

	window.min = Pt(48, 48);
	window.max = addpt(window.min, Pt(10+w*80, 10+h*50));
	if(window.max.y >= scr->gscreen->r.max.y)
		window.max.y = scr->gscreen->r.max.y-1;
	if(window.max.x >= scr->gscreen->r.max.x)
		window.max.x = scr->gscreen->r.max.x-1;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	curpos = window.min;

	screenputs = vgascreenputs;
}