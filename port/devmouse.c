#include	"u.h"
#include	"../port/lib.h"
#include	<libg.h>
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"devtab.h"
#include	"screen.h"

typedef struct Mouseinfo	Mouseinfo;

struct Mouseinfo
{
	/*
	 * First three fields are known in some l.s's
	 */
	int	dx;
	int	dy;
	int	track;		/* l.s has updated dx & dy */
	Mouse;
	int	redraw;		/* update cursor on screen */
	ulong	counter;	/* increments every update */
	ulong	lastcounter;	/* value when /dev/mouse read */
	Rendez	r;
	Ref;
	QLock;
	int	open;
};

Mouseinfo	mouse;
Cursorinfo	cursor;
int		mouseshifted;
int		mousetype;
int		mouseswap;
Cursor		curs;

Cursor	arrow = {
	{ -1, -1 },
	{ 0xFF, 0xFF, 0x80, 0x01, 0x80, 0x02, 0x80, 0x0C, 
	  0x80, 0x10, 0x80, 0x10, 0x80, 0x08, 0x80, 0x04, 
	  0x80, 0x02, 0x80, 0x01, 0x80, 0x02, 0x8C, 0x04, 
	  0x92, 0x08, 0x91, 0x10, 0xA0, 0xA0, 0xC0, 0x40, 
	},
	{ 0x00, 0x00, 0x7F, 0xFE, 0x7F, 0xFC, 0x7F, 0xF0, 
	  0x7F, 0xE0, 0x7F, 0xE0, 0x7F, 0xF0, 0x7F, 0xF8, 
	  0x7F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFC, 0x73, 0xF8, 
	  0x61, 0xF0, 0x60, 0xE0, 0x40, 0x40, 0x00, 0x00, 
	},
};

void	Cursortocursor(Cursor*);
int	mousechanged(void*);

enum{
	Qdir,
	Qcursor,
	Qmouse,
	Qmousectl,
};

Dirtab mousedir[]={
	"cursor",	{Qcursor},	0,			0666,
	"mouse",	{Qmouse},	0,			0666,
	"mousectl",	{Qmousectl},	0,			0220,
};

#define	NMOUSE	(sizeof(mousedir)/sizeof(Dirtab))

extern	Bitmap	gscreen;

void
mousereset(void)
{
	if(!conf.monitor)
		return;

	curs = arrow;
	Cursortocursor(&arrow);
}

void
mouseinit(void)
{
	if(!conf.monitor)
		return;

	cursoron(1);
}

Chan*
mouseattach(char *spec)
{
	if(!conf.monitor)
		error(Egreg);
	return devattach('m', spec);
}

Chan*
mouseclone(Chan *c, Chan *nc)
{
	nc = devclone(c, nc);
	if(c->qid.path != CHDIR)
		incref(&mouse);
	return nc;
}

int
mousewalk(Chan *c, char *name)
{
	return devwalk(c, name, mousedir, NMOUSE, devgen);
}

void
mousestat(Chan *c, char *db)
{
	devstat(c, db, mousedir, NMOUSE, devgen);
}

Chan*
mouseopen(Chan *c, int omode)
{
	switch(c->qid.path){
	case CHDIR:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qmouse:
		lock(&mouse);
		if(mouse.open){
			unlock(&mouse);
			error(Einuse);
		}
		mouse.open = 1;
		mouse.ref++;
		unlock(&mouse);
		break;
	default:
		incref(&mouse);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
mousecreate(Chan *c, char *name, int omode, ulong perm)
{
	if(!conf.monitor)
		error(Egreg);
	USED(c, name, omode, perm);
	error(Eperm);
}

void
mouseremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
mousewstat(Chan *c, char *db)
{
	USED(c, db);
	error(Eperm);
}

void
mouseclose(Chan *c)
{
	if(c->qid.path!=CHDIR && (c->flag&COPEN)){
		lock(&mouse);
		if(c->qid.path == Qmouse)
			mouse.open = 0;
		if(--mouse.ref == 0){
			cursoroff(1);
			curs = arrow;
			Cursortocursor(&arrow);
			cursoron(1);
		}
		unlock(&mouse);
	}
}


long
mouseread(Chan *c, void *va, long n, ulong offset)
{
	char buf[4*12+1];
	uchar *p;
	static int map[8] = {0, 4, 2, 6, 1, 5, 3, 7 };

	p = va;
	switch(c->qid.path){
	case CHDIR:
		return devdirread(c, va, n, mousedir, NMOUSE, devgen);

	case Qcursor:
		if(offset != 0)
			return 0;
		if(n < 2*4+2*2*16)
			error(Eshort);
		n = 2*4+2*2*16;
		lock(&cursor);
		BPLONG(p+0, curs.offset.x);
		BPLONG(p+4, curs.offset.y);
		memmove(p+8, curs.clr, 2*16);
		memmove(p+40, curs.set, 2*16);
		unlock(&cursor);
		return n;

	case Qmouse:
		while(mousechanged(0) == 0)
			sleep(&mouse.r, mousechanged, 0);
		lock(&cursor);
		sprint(buf, "m%11d %11d %11d %11d",
			mouse.xy.x, mouse.xy.y,
			mouseswap ? map[mouse.buttons&7] : mouse.buttons,
			TK2MS(MACHP(0)->ticks));
		mouse.lastcounter = mouse.counter;
		unlock(&cursor);
		if(n > 1+4*12)
			n = 1+4*12;
		memmove(va, buf, n);
		return n;
	}
	return 0;
}

Block*
mousebread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
mousewrite(Chan *c, void *va, long n, ulong offset)
{
	char *p;
	Point pt;
	char buf[64];

	USED(offset);
	p = va;
	switch(c->qid.path){
	case CHDIR:
		error(Eisdir);

	case Qcursor:
		cursoroff(1);
		if(n < 2*4+2*2*16){
			curs = arrow;
			Cursortocursor(&arrow);
		}else{
			n = 2*4+2*2*16;
			curs.offset.x = BGLONG(p+0);
			curs.offset.y = BGLONG(p+4);
			memmove(curs.clr, p+8, 2*16);
			memmove(curs.set, p+40, 2*16);
			Cursortocursor(&curs);
		}
		qlock(&mouse);
		mouse.redraw = 1;
		mouseclock();
		qunlock(&mouse);
		cursoron(1);
		return n;

	case Qmousectl:
		if(n >= sizeof(buf))
			n = sizeof(buf)-1;
		strncpy(buf, va, n);
		buf[n] = 0;
		mousectl(buf);
		return n;

	case Qmouse:
		if(n > sizeof buf-1)
			n = sizeof buf -1;
		memmove(buf, va, n);
		buf[n] = 0;
		p = 0;
		pt.x = strtoul(buf+1, &p, 0);
		if(p == 0)
			error(Eshort);
		pt.y = strtoul(p, 0, 0);
		qlock(&mouse);
		if(ptinrect(pt, gscreen.r)){
			mouse.xy = pt;
			mouse.redraw = 1;
			mouse.track = 1;
			mouseclock();
		}
		qunlock(&mouse);
		return n;
	}

	error(Egreg);
	return -1;
}

long
mousebwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void
Cursortocursor(Cursor *c)
{
	lock(&cursor);
	memmove(&cursor.Cursor, c, sizeof(Cursor));
	setcursor(c);
	unlock(&cursor);
}


/*
 *  called by the clock routine to redraw the cursor
 */
void
mouseclock(void)
{
	if(mouse.track){
		mousetrack(mouse.buttons, mouse.dx, mouse.dy);
		mouse.track = 0;
		mouse.dx = 0;
		mouse.dy = 0;
	}
	if(mouse.redraw && canlock(&cursor)){
		mouse.redraw = 0;
		cursoroff(0);
		mouse.redraw = cursoron(0);
		unlock(&cursor);
	}
	graphicsactive(0);
}

/*
 *  called at interrupt level to update the structure and
 *  awaken any waiting procs.
 */
void
mousetrack(int b, int dx, int dy)
{
	int x, y;

	x = mouse.xy.x + dx;
	if(x < gscreen.r.min.x)
		x = gscreen.r.min.x;
	if(x >= gscreen.r.max.x)
		x = gscreen.r.max.x;
	y = mouse.xy.y + dy;
	if(y < gscreen.r.min.y)
		y = gscreen.r.min.y;
	if(y >= gscreen.r.max.y)
		y = gscreen.r.max.y;
	mouse.counter++;
	mouse.xy = Pt(x, y);
	mouse.buttons = b;
	mouse.redraw = 1;
	wakeup(&mouse.r);
	graphicsactive(1);
}

/*
 *  microsoft 3 button, 7 bit bytes
 *
 *	byte 0 -	1  L  R Y7 Y6 X7 X6
 *	byte 1 -	0 X5 X4 X3 X2 X1 X0
 *	byte 2 -	0 Y5 Y4 Y3 Y2 Y1 Y0
 *	byte 3 -	0  M  x  x  x  x  x	(optional)
 *
 *  shift & right button is the same as middle button (for 2 button mice)
 */
int
m3mouseputc(void *q, int c)
{
	static uchar msg[3];
	static int nb;
	static int middle;
	static uchar b[] = { 0, 4, 1, 5, 0, 2, 1, 5 };
	short x;
	int dx, dy, newbuttons;

	USED(q);

	/* 
	 *  check bit 6 for consistency
	 */
	if(nb==0){
		if((c&0x40) == 0){
			/* an extra byte gets sent for the middle button */
			middle = (c&0x20) ? 2 : 0;
			newbuttons = (mouse.buttons & ~2) | middle;
			mousetrack(newbuttons, 0, 0);
			return 0;
		}
	}
	msg[nb] = c;
	if(++nb == 3){
		nb = 0;
		newbuttons = middle | b[(msg[0]>>4)&3 | (mouseshifted ? 4 : 0)];
		x = (msg[0]&0x3)<<14;
		dx = (x>>8) | msg[1];
		x = (msg[0]&0xc)<<12;
		dy = (x>>8) | msg[2];
		mousetrack(newbuttons, dx, dy);
	}
	return 0;
}

/*
 *  Logitech 5 byte packed binary mouse format, 8 bit bytes
 *
 *  shift & right button is the same as middle button (for 2 button mice)
 */
int
mouseputc(void *q, int c)
{
	static short msg[5];
	static int nb;
	static uchar b[] = {0, 4, 2, 6, 1, 5, 3, 7, 0, 2, 2, 6, 1, 5, 3, 7};
	int dx, dy, newbuttons;

	USED(q);
	if((c&0xF0) == 0x80)
		nb=0;
	msg[nb] = c;
	if(c & 0x80)
		msg[nb] |= ~0xFF;	/* sign extend */
	if(++nb == 5){
		newbuttons = b[((msg[0]&7)^7) | (mouseshifted ? 8 : 0)];
		dx = msg[1]+msg[3];
		dy = -(msg[2]+msg[4]);
		mousetrack(newbuttons, dx, dy);
		nb = 0;
	}
	return 0;
}

int
mousechanged(void *m)
{
	USED(m);
	return mouse.lastcounter - mouse.counter;
}

Point
mousexy(void)
{
	return mouse.xy;
}
