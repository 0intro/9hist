#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"gnot.h"

extern Font	defont0;	/* BUG */

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

struct{
	Ref;
	int	bltuse;
	Bitmap	*map;		/* arena */
	Bitmap	*free;		/* free list */
	ulong	*words;		/* storage */
	ulong	nwords;		/* total in arena */
	ulong	*wfree;		/* pointer to next free word */
	int	lastid;		/* last allocated bitmap id */
	int	init;		/* freshly opened; init message pending */
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
	Rendez	r;
}mouse;

struct{
	Cursor;
	Lock;
	int	visible;	/* on screen */
	Rectangle r;		/* location */
}cursor =
{
	{{0, 0},
	{0xFFE0, 0xFFE0, 0xFFC0, 0xFF00,
	 0xFF00, 0xFF80, 0xFFC0, 0xFFE0,
	 0xE7F0, 0xE3F8, 0xC1FC, 0x00FE,
	 0x007F, 0x003E, 0x001C, 0x0008,
	},
	{0x0000, 0x7FC0, 0x7F00, 0x7C00,
	 0x7E00, 0x7F00, 0x6F80, 0x67C0,
	 0x43E0, 0x41F0, 0x00F8, 0x007C,
	 0x003E, 0x001C, 0x0008, 0x0000,
	}},
};

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

void	cursortobitmap(void);
void	cursoron(int);
void	cursoroff(int);

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
	cursortobitmap();
}

void
bitinit(void)
{
	lock(&bit);
	bit.bltuse = 0;
	unlock(&bit);
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
	}else{
		lock(&bit);
		if(bit.bltuse){
			unlock(&bit);
			error(0, Einuse);
		}
		bit.lastid = -1;
		bit.init = 1;
		unlock(&bit);
		incref(&bit);
	}
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

	if(c->qid != CHDIR){
		lock(&bit);
		if(--bit.ref == 0){
			for(i=1,bp=&bit.map[1]; i<conf.nbitmap; i++,bp++)
				if(bp->ldepth >= 0)
					bitfree(bp);
			bit.bltuse = 0;
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
	uchar *p;

	if(c->qid & CHDIR)
		return devdirread(c, va, n, bitdir, NBIT, devgen);

	if(c->qid != Qbitblt)
		error(0, Egreg);
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
		goto done;
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
		goto done;
	}
	error(0, Ebadblt);

    done:
	return n;
}


long
bitwrite(Chan *c, void *va, long n)
{
	uchar *p, *q;
	long m, v, miny, maxy, t, x, y;
	ulong l, nw, ws;
	int off;
	Point pt;
	Rectangle rect;
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
			if(v != 0)	/* BUG */
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
			if(bit.wfree+l+2 > bit.words+bit.nwords){
				bitcompact();
				if(bit.wfree+l+1 > bit.words+bit.nwords)
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

		case 's':
			/*
			 * string
			 *	's'		1
			 *	id		2
			 *	pt		8
			 *	font id		2
			 *	fcode		2
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
			string(dst, pt, &defont0/*BUG*/, (char*)p, v);
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
	bp->base[1] = 0;
	bp->ldepth = -1;
	bp->base = (ulong*)bit.free;
	bit.free = bp;
}

void
bitcompact(void)
{
	ulong *p1, *p2;

print("bitcompact\n");
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
print("bitcompact done\n");
}

void
cursortobitmap(void)
{
	int i;

	lock(&cursor);
	for(i=0; i<16; i++){
		setbits[i] = cursor.set[i]<<16;
		clrbits[i] = cursor.clr[i]<<16;
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
mousebuttons(int b)	/* called splhi */
{
	mouse.buttons = b;
	mouse.changed = 1;
	wakeup(&mouse.r);
}

void
mouseclock(void)	/* called splhi */
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
		mouse.changed = 1;
		unlock(&cursor);
		wakeup(&mouse.r);
	}
}
