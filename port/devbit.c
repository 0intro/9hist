#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"gnot.h"

extern Font	*defont;

/*
 * Device (#b/bitblt) is exclusive use on open, so no locks are necessary
 * for i/o
 */

/*
 * Some fields in Bitmaps are overloaded:
 *	ldepth = -1 means free.
 *	base is next pointer when free.
 * Arena is a word containing N, followed by a pointer to its bitmap,
 * followed by N blocks.  The bitmap pointer is zero if block is free. 
 */

struct
{
	Ref;
	Bitmap	*map;		/* arena */
	Bitmap	*free;		/* free list */
	ulong	*words;		/* storage */
	ulong	nwords;		/* total in arena */
	ulong	*wfree;		/* pointer to next free word */
	int	lastid;		/* last allocated bitmap id */
	int	init;		/* freshly opened; init message pending */
	int	rid;		/* read bitmap id */
	int	rminy;		/* read miny */
	int	rmaxy;		/* read maxy */
}bit;

#define	FREE	0x80000000
void	bitcompact(void);
void	bitfree(Bitmap*);
extern	Bitmap	screen;

struct{
	/*
	 * First three fields are known in l.s
	 */
	int	dx;		/* interrupt-time delta */
	int	dy;
	int	track;		/* update cursor on screen */
	Mouse;
	int	changed;	/* mouse structure changed since last read */
	int	newbuttons;	/* interrupt time access only */
	Rendez	r;
}mouse;

Cursor	arrow =
{
	{0, 0},
	{0xFF, 0xE0, 0xFF, 0xE0, 0xFF, 0xC0, 0xFF, 0x00,
	 0xFF, 0x00, 0xFF, 0x80, 0xFF, 0xC0, 0xFF, 0xE0,
	 0xE7, 0xF0, 0xE3, 0xF8, 0xC1, 0xFC, 0x00, 0xFE,
	 0x00, 0x7F, 0x00, 0x3E, 0x00, 0x1C, 0x00, 0x08,
	},
	{0x00, 0x00, 0x7F, 0xC0, 0x7F, 0x00, 0x7C, 0x00,
	 0x7E, 0x00, 0x7F, 0x00, 0x6F, 0x80, 0x67, 0xC0,
	 0x43, 0xE0, 0x41, 0xF0, 0x00, 0xF8, 0x00, 0x7C,
	 0x00, 0x3E, 0x00, 0x1C, 0x00, 0x08, 0x00, 0x00,
	}
};

struct{
	Cursor;
	Lock;
	int	visible;	/* on screen */
	Rectangle r;		/* location */
}cursor;

ulong setbits[16];
Bitmap	set =
{
	setbits,
	0,
	1,
	0,
	{0, 0, 16, 16}
};

ulong clrbits[16];
Bitmap	clr =
{
	clrbits,
	0,
	1,
	0,
	{0, 0, 16, 16}
};

ulong cursorbackbits[16];
Bitmap cursorback =
{
	cursorbackbits,
	0,
	1,
	0,
	{0, 0, 16, 16}
};

void	Cursortocursor(Cursor*);
void	cursoron(int);
void	cursoroff(int);
int	mousechanged(void*);

enum{
	Qdir,
	Qbitblt,
	Qmouse,
};

Dirtab bitdir[]={
	"bitblt",	Qbitblt,	0,			0600,
	"mouse",	Qmouse,		0,			0600,
};

#define	NBIT	(sizeof bitdir/sizeof(Dirtab))

void
bitreset(void)
{
	int i;
	Bitmap *bp;

	bit.map = ialloc(conf.nbitmap * sizeof(Bitmap), 0);
	for(i=0,bp=bit.map; i<conf.nbitmap; i++,bp++){
		bp->ldepth = -1;
		bp->base = (ulong*)(bp+1);
	}
	bp--;
	bp->base = 0;
	bit.map[0] = screen;	/* bitmap 0 is screen */
	bit.free = bit.map+1;
	bit.lastid = -1;
	bit.words = ialloc(conf.nbitbyte, 0);
	bit.nwords = conf.nbitbyte/sizeof(ulong);
	bit.wfree = bit.words;
	Cursortocursor(&arrow);
}

void
bitinit(void)
{
	lock(&bit);
	unlock(&bit);
	if(screen.ldepth > 1)
		panic("bitinit ldepth>1");
	cursorback.ldepth = screen.ldepth;
	cursoron(1);
}

Chan*
bitattach(char *spec)
{
	return devattach('b', spec);
}

Chan*
bitclone(Chan *c, Chan *nc)
{
	nc = devclone(c, nc);
	if(c->qid != CHDIR)
		incref(&bit);
}

int
bitwalk(Chan *c, char *name)
{
	return devwalk(c, name, bitdir, NBIT, devgen);
}

void
bitstat(Chan *c, char *db)
{
	devstat(c, db, bitdir, NBIT, devgen);
}

Chan *
bitopen(Chan *c, int omode)
{
	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eperm);
	}else if(c->qid == Qbitblt){
		lock(&bit);
		if(bit.ref){
			unlock(&bit);
			error(0, Einuse);
		}
		bit.lastid = -1;
		bit.rid = -1;
		bit.init = 0;
		bit.ref = 1;
		Cursortocursor(&arrow);
		unlock(&bit);
	}else
		incref(&bit);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
bitcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void
bitremove(Chan *c)
{
	error(0, Eperm);
}

void
bitwstat(Chan *c, char *db)
{
	error(0, Eperm);
}

void
bitclose(Chan *c)
{
	int i;
	Bitmap *bp;

	if(c->qid!=CHDIR && (c->flag&COPEN)){
		lock(&bit);
		if(--bit.ref == 0){
			for(i=1,bp=&bit.map[1]; i<conf.nbitmap; i++,bp++)
				if(bp->ldepth >= 0)
					bitfree(bp);
		}
		unlock(&bit);
	}
}

#define	GSHORT(p)		(((p)[0]<<0) | ((p)[1]<<8))
#define	GLONG(p)		((GSHORT(p)<<0) | (GSHORT(p+2)<<16))
#define	PSHORT(p, v)		((p)[0]=(v), (p)[1]=((v)>>8))
#define	PLONG(p, v)		(PSHORT(p, (v)), PSHORT(p+2, (v)>>16))

/*
 * These macros turn user-level (high bit at left) into internal (whatever)
 * bit order.  On the gnot they're trivial.
 */
#define	U2K(x)	(x)
#define	K2U(x)	(x)

long
bitread(Chan *c, void *va, long n)
{
	uchar *p, *q;
	long miny, maxy, t, x, y;
	ulong l, nw, ws;
	int off;
	Bitmap *src;

	if(c->qid & CHDIR)
		return devdirread(c, va, n, bitdir, NBIT, devgen);

	switch(c->qid){
	case Qmouse:
		/*
		 * mouse:
		 *	'm'		1
		 *	buttons		1
		 * 	point		8
		 */
		if(n < 10)
			error(0, Ebadblt);
	    Again:
		while(mouse.changed == 0)
			sleep(&mouse.r, mousechanged, 0);
		lock(&cursor);
		if(mouse.changed == 0){
			unlock(&cursor);
			goto Again;
		}
		p = va;
		p[0] = 'm';
		p[1] = mouse.buttons;
		PLONG(p+2, mouse.xy.x);
		PLONG(p+6, mouse.xy.y);
		mouse.changed = 0;
		unlock(&cursor);
		n = 10;
		break;

	case Qbitblt:
		p = va;
		/*
		 * Fuss about and figure out what to say.
		 */
		if(bit.init){
			/*
			 * init:
			 *	'I'		1
			 *	ldepth		1
			 * 	rectangle	16
			 */
			if(n < 18)
				error(0, Ebadblt);
			p[0] = 'I';
			p[1] = screen.ldepth;
			PLONG(p+2, screen.r.min.x);
			PLONG(p+6, screen.r.min.y);
			PLONG(p+10, screen.r.max.x);
			PLONG(p+14, screen.r.max.y);
			bit.init = 0;
			n = 18;
			break;
		}
		if(bit.lastid > 0){
			/*
			 * allocate:
			 *	'A'		1
			 *	bitmap id	2
			 */
			if(n < 3)
				error(0, Ebadblt);
			p[0] = 'A';
			PSHORT(p+1, bit.lastid);
			bit.lastid = -1;
			n = 3;
			break;
		}
		if(bit.rid >= 0){
			/*
			 * read
			 *	data		bytewidth*(maxy-miny)
			 */
			src = &bit.map[bit.rid];
			if(src->ldepth<0)
				error(0, Ebadbitmap);
			off = 0;
			if(bit.rid == 0)
				off = 1;
			miny = bit.rminy;
			maxy = bit.rmaxy;
			if(miny>maxy || miny<src->r.min.y || maxy>src->r.max.y)
				error(0, Ebadblt);
			ws = 1<<(3-src->ldepth);	/* pixels per byte */
			/* set l to number of bytes of incoming data per scan line */
			if(src->r.min.x >= 0)
				l = (src->r.max.x+ws-1)/ws - src->r.min.x/ws;
			else{	/* make positive before divide */
				t = (-src->r.min.x)+ws-1;
				t = (t/ws)*ws;
				l = (t+src->r.max.x+ws-1)/ws;
			}
			if(n < l*(maxy-miny))
				error(0, Ebadblt);
			if(off)
				cursoroff(1);
			n = 0;
			p = va;
			for(y=miny; y<maxy; y++){
				q = (uchar*)addr(src, Pt(src->r.min.x, y));
				q += (src->r.min.x&((sizeof(ulong))*ws-1))/8;
				for(x=0; x<l; x++)
					*p++ = K2U(*q++);
				n += l;
			}
			if(off)
				cursoron(1);
			bit.rid = -1;
			break;
		}
		error(0, Ebadblt);

	default:
		error(0, Egreg);
	}

	return n;
}


long
bitwrite(Chan *c, void *va, long n)
{
	uchar *p, *q;
	long m, v, miny, maxy, t, x, y;
	ulong l, nw, ws;
	int off;
	Point pt, pt1, pt2;
	Rectangle rect;
	Cursor curs;
	Bitmap *bp, *src, *dst;

	if(c->qid == CHDIR)
		error(0, Eisdir);

	if(c->qid != Qbitblt)
		error(0, Egreg);

	p = va;
	m = n;
	while(m > 0)
		switch(*p){
		default:
			pprint("bitblt request 0x%x\n", *p);
			error(0, Ebadblt);

		case 'a':
			/*
			 * allocate:
			 *	'a'		1
			 *	ldepth		1
			 *	Rectangle	16
			 * next read returns allocated bitmap id
			 */
			if(m < 18)
				error(0, Ebadblt);
			v = *(p+1);
			if(v!=0 && v!=1)	/* BUG */
				error(0, Ebadblt);
			ws = 1<<(5-v);	/* pixels per word */
			if(bit.free == 0)
				error(0, Enobitmap);
			rect.min.x = GLONG(p+2);
			rect.min.y = GLONG(p+6);
			rect.max.x = GLONG(p+10);
			rect.max.y = GLONG(p+14);
			if(rect.min.x >= 0)
				l = (rect.max.x+ws-1)/ws - rect.min.x/ws;
			else{	/* make positive before divide */
				t = (-rect.min.x)+ws-1;
				t = (t/ws)*ws;
				l = (t+rect.max.x+ws-1)/ws;
			}
			nw = l*Dy(rect);
			if(bit.wfree+2+nw > bit.words+bit.nwords){
				bitcompact();
				if(bit.wfree+2+nw > bit.words+bit.nwords)
					error(0, Enobitstore);
			}
			bp = bit.free;
			bit.free = (Bitmap*)(bp->base);
			*bit.wfree++ = nw;
			*bit.wfree++ = (ulong)bp;
			bp->base = bit.wfree;
			memset(bp->base, 0, nw*sizeof(ulong));
			bit.wfree += nw;
			bp->zero = l*rect.min.y;
			if(rect.min.x >= 0)
				bp->zero += rect.min.x/ws;
			else
				bp->zero -= (-rect.min.x+ws-1)/ws;
			bp->zero = -bp->zero;
			bp->width = l;
			bp->ldepth = v;
			bp->r = rect;
			bp->cache = 0;
			bit.lastid = bp-bit.map;
			m -= 18;
			p += 18;
			break;

		case 'b':
			/*
			 * bitblt
			 *	'b'		1
			 *	dst id		2
			 *	dst Point	8
			 *	src id		2
			 *	src Rectangle	16
			 *	code		2
			 */
			if(m < 31)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth < 0)
				error(0, Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			pt.x = GLONG(p+3);
			pt.y = GLONG(p+7);
			v = GSHORT(p+11);
			src = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || src->ldepth < 0)
				error(0, Ebadbitmap);
			if(v == 0)
				off = 1;
			rect.min.x = GLONG(p+13);
			rect.min.y = GLONG(p+17);
			rect.max.x = GLONG(p+21);
			rect.max.y = GLONG(p+25);
			v = GSHORT(p+29);
			if(off)
				cursoroff(1);
			bitblt(dst, pt, src, rect, v);
			if(off)
				cursoron(1);
			m -= 31;
			p += 31;
			break;

		case 'c':
			/*
			 * cursorswitch
			 *	'c'		1
			 * nothing more: return to arrow; else
			 * 	Point		8
			 *	clr		32
			 *	set		32
			 */
			if(m == 1){
				cursoroff(1);
				Cursortocursor(&arrow);
				cursoron(1);
				m -= 1;
				p += 1;
				break;
			}
			if(m < 73)
				error(0, Ebadblt);
			curs.offset.x = GLONG(p+1);
			curs.offset.y = GLONG(p+5);
			memcpy(curs.clr, p+9, 2*16);
			memcpy(curs.set, p+41, 2*16);
			cursoroff(1);
			Cursortocursor(&curs);
			cursoron(1);
			m -= 73;
			p += 73;
			break;

		case 'f':
			/*
			 * free
			 *	'f'		1
			 *	id		2
			 */
			if(m < 3)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v>=conf.nbitmap || dst->ldepth<0)
				error(0, Ebadbitmap);
			bitfree(dst);
			m -= 3;
			p += 3;
			break;

		case 'i':
			/*
			 * init
			 *
			 *	'i'		1
			 */
			bit.init = 1;
			m -= 1;
			p += 1;
			break;

		case 'l':
			/*
			 * line segment
			 *
			 *	'l'		1
			 *	id		2
			 *	pt1		8
			 *	pt2		8
			 *	value		1
			 *	code		2
			 */
			if(m < 22)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v>=conf.nbitmap || dst->ldepth<0)
				error(0, Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			pt1.x = GLONG(p+3);
			pt1.y = GLONG(p+7);
			pt2.x = GLONG(p+11);
			pt2.y = GLONG(p+15);
			t = p[19];
			v = GSHORT(p+20);
			if(off)
				cursoroff(1);
			segment(dst, pt1, pt2, t, v);
			if(off)
				cursoron(1);
			m -= 22;
			p += 22;
			break;

		case 'r':
			/*
			 * read
			 *	'r'		1
			 *	src id		2
			 *	miny		4
			 *	maxy		4
			 */
			if(m < 11)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			src = &bit.map[v];
			if(v>=conf.nbitmap || src->ldepth<0)
				error(0, Ebadbitmap);
			miny = GLONG(p+3);
			maxy = GLONG(p+7);
			if(miny>maxy || miny<src->r.min.y || maxy>src->r.max.y)
				error(0, Ebadblt);
			bit.rid = v;
			bit.rminy = miny;
			bit.rmaxy = maxy;
			p += 11;
			m -= 11;
			break;

		case 's':
			/*
			 * string
			 *	's'		1
			 *	id		2
			 *	pt		8
			 *	font id		2
			 *	code		2
			 * 	string		n (null terminated)
			 */
			if(m < 16)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v>=conf.nbitmap || dst->ldepth<0)
				error(0, Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			pt.x = GLONG(p+3);
			pt.y = GLONG(p+7);
			v = GSHORT(p+11);
			if(v != 0)	/* BUG */
				error(0, Ebadblt);
			v = GSHORT(p+13);
			p += 15;
			m -= 15;
			q = memchr(p, 0, m);
			if(q == 0)
				error(0, Ebadblt);
			if(off)
				cursoroff(1);
			string(dst, pt, defont/*BUG*/, (char*)p, v);
			if(off)
				cursoron(1);
			q++;
			m -= q-p;
			p = q;
			break;

		case 't':
			/*
			 * texture
			 *	't'		1
			 *	dst id		2
			 *	Rectangle	16
			 *	src id		2
			 *	fcode		2
			 */
			if(m < 23)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v>=conf.nbitmap || dst->ldepth<0)
				error(0, Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			rect.min.x = GLONG(p+3);
			rect.min.y = GLONG(p+7);
			rect.max.x = GLONG(p+11);
			rect.max.y = GLONG(p+15);
			v = GSHORT(p+19);
			src = &bit.map[v];
			if(v>=conf.nbitmap || src->ldepth<0)
				error(0, Ebadbitmap);
			if(src->r.min.x!=0 || src->r.min.y!=0 || src->r.max.x!=16 || src->r.max.y!=16)
				error(0, Ebadblt);
			v = GSHORT(p+21);
			{
				int i;
				Texture t;

				for(i=0; i<16; i++)
					t.bits[i] = src->base[i]>>16;
				if(off)
					cursoroff(1);
				texture(dst, rect, &t, v);
				if(off)
					cursoron(1);
			}
			m -= 23;
			p += 23;
			break;

		case 'w':
			/*
			 * write
			 *	'w'		1
			 *	dst id		2
			 *	miny		4
			 *	maxy		4
			 *	data		bytewidth*(maxy-miny)
			 */
			if(m < 11)
				error(0, Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v>=conf.nbitmap || dst->ldepth<0)
				error(0, Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			miny = GLONG(p+3);
			maxy = GLONG(p+7);
			if(miny>maxy || miny<dst->r.min.y || maxy>dst->r.max.y)
				error(0, Ebadblt);
			ws = 1<<(3-dst->ldepth);	/* pixels per byte */
			/* set l to number of bytes of incoming data per scan line */
			if(dst->r.min.x >= 0)
				l = (dst->r.max.x+ws-1)/ws - dst->r.min.x/ws;
			else{	/* make positive before divide */
				t = (-dst->r.min.x)+ws-1;
				t = (t/ws)*ws;
				l = (t+dst->r.max.x+ws-1)/ws;
			}
			p += 11;
			m -= 11;
			if(m < l*(maxy-miny))
				error(0, Ebadblt);
			if(off)
				cursoroff(1);
			for(y=miny; y<maxy; y++){
				q = (uchar*)addr(dst, Pt(dst->r.min.x, y));
				q += (dst->r.min.x&((sizeof(ulong))*ws-1))/8;
				for(x=0; x<l; x++)
					*q++ = U2K(*p++);
				m -= l;
			}
			if(off)
				cursoron(1);
			break;
		}

	return n;
}

void
bituserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void
biterrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void
bitfree(Bitmap *bp)
{
	bp->base[-1] = 0;
	bp->ldepth = -1;
	bp->base = (ulong*)bit.free;
	bit.free = bp;
}

void
bitcompact(void)
{
	ulong *p1, *p2;

	p1 = p2 = bit.words;
	while(p2 < bit.wfree){
		if(p2[1] == 0){
			p2 += 2 + p2[0];
			continue;
		}
		if(p1 != p2){
			memcpy(p1, p2, (2+p2[0])*sizeof(ulong));
			((Bitmap*)p1[1])->base = p1+2;
		}
		p2 += 2 + p1[0];
		p1 += 2 + p1[0];
	}
	bit.wfree = p1;
}

void
Cursortocursor(Cursor *c)
{
	int i;

	lock(&cursor);
	memcpy(&cursor, c, sizeof(Cursor));
	for(i=0; i<16; i++){
		setbits[i] = (c->set[2*i]<<24) + (c->set[2*i+1]<<16);
		clrbits[i] = (c->clr[2*i]<<24) + (c->clr[2*i+1]<<16);
	}
	unlock(&cursor);
}

void
cursoron(int dolock)
{
	if(dolock)
		lock(&cursor);
	if(cursor.visible++ == 0){
		cursor.r.min = mouse.xy;
		cursor.r.max = add(mouse.xy, Pt(16, 16));
		cursor.r = raddp(cursor.r, cursor.offset);
		bitblt(&cursorback, Pt(0, 0), &screen, cursor.r, S);
		bitblt(&screen, add(mouse.xy, cursor.offset),
			&clr, Rect(0, 0, 16, 16), D&~S);
		bitblt(&screen, add(mouse.xy, cursor.offset),
			&set, Rect(0, 0, 16, 16), S|D);
	}
	if(dolock)
		unlock(&cursor);
}

void
cursoroff(int dolock)
{
	if(dolock)
		lock(&cursor);
	if(--cursor.visible == 0)
		bitblt(&screen, cursor.r.min, &cursorback, Rect(0, 0, 16, 16), S);
	if(dolock)
		unlock(&cursor);
}

void
mousebuttons(int b)	/* called spl5 */
{
	/*
	 * It is possible if you click very fast and get bad luck
	 * you could miss a button click (down up).  Doesn't seem
	 * likely or important enough to worry about.
	 */
	mouse.newbuttons = b;
	mouse.track = 1;		/* aggressive but o.k. */
	mouseclock();
}

void
mouseclock(void)	/* called spl6 */
{
	int x, y;
	if(mouse.track && canlock(&cursor)){
		x = mouse.xy.x + mouse.dx;
		if(x < screen.r.min.x)
			x = screen.r.min.x;
		if(x >= screen.r.max.x)
			x = screen.r.max.x;
		y = mouse.xy.y + mouse.dy;
		if(y < screen.r.min.y)
			y = screen.r.min.y;
		if(y >= screen.r.max.y)
			y = screen.r.max.y;
		cursoroff(0);
		mouse.xy = Pt(x, y);
		cursoron(0);
		mouse.dx = 0;
		mouse.dy = 0;
		mouse.track = 0;
		mouse.buttons = mouse.newbuttons;
		mouse.changed = 1;
		unlock(&cursor);
		wakeup(&mouse.r);
	}
}

int
mousechanged(void *m)
{
	return mouse.changed;
}
