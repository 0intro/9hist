#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	<libg.h>
#include	<gnot.h>
#include	"screen.h"

extern GFont	*defont;

/*
 * Some monochrome screens are reversed from what we like:
 * We want 0's bright and 1s dark.
 * Indexed by an Fcode, these compensate for the source bitmap being wrong
 * (exchange S rows) and destination (exchange D columns and invert result)
 */
int flipS[] = {
	0x0, 0x4, 0x8, 0xC, 0x1, 0x5, 0x9, 0xD,
	0x2, 0x6, 0xA, 0xE, 0x3, 0x7, 0xB, 0xF
};

int flipD[] = {
	0xF, 0xD, 0xE, 0xC, 0x7, 0x5, 0x6, 0x4,
	0xB, 0x9, 0xA, 0x8, 0x3, 0x1, 0x2, 0x0, 
};

int flipping;	/* are flip tables being used to transform Fcodes? */
int hwcursor;	/* is there a hardware cursor? */

/*
 * Device (#b/bitblt) is exclusive use on open, so no locks are necessary
 * for i/o
 */

/*
 * Some fields in GBitmaps are overloaded:
 *	ldepth = -1 means free.
 *	base is next pointer when free.
 * Arena is a word containing N, followed by a pointer to its bitmap,
 * followed by N blocks.  The bitmap pointer is zero if block is free. 
 */

struct
{
	Ref;
	GBitmap	*map;		/* arena */
	GBitmap	*free;		/* free list */
	ulong	*words;		/* storage */
	ulong	nwords;		/* total in arena */
	ulong	*wfree;		/* pointer to next free word */
	GFont	*font;		/* arena; looked up linearly BUG */
	int	lastid;		/* last allocated bitmap id */
	int	lastfid;	/* last allocated font id */
	int	init;		/* freshly opened; init message pending */
	int	rid;		/* read bitmap id */
	int	rminy;		/* read miny */
	int	rmaxy;		/* read maxy */
	int	mid;		/* colormap read bitmap id */
}bit;

#define	FREE	0x80000000
void	bitcompact(void);
void	bitfree(GBitmap*);
extern	GBitmap	gscreen;

Mouseinfo	mouse;
Cursorinfo	cursor;

Cursor	arrow =
{
	{-1, -1},
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

ulong setbits[16];
GBitmap	set =
{
	setbits,
	0,
	1,
	0,
	{0, 0, 16, 16}
};

ulong clrbits[16];
GBitmap	clr =
{
	clrbits,
	0,
	1,
	0,
	{0, 0, 16, 16}
};

ulong cursorbackbits[16*4];
GBitmap cursorback =
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
	Qscreen,
};

Dirtab bitdir[]={
	"bitblt",	{Qbitblt},	0,			0666,
	"mouse",	{Qmouse},	0,			0666,
	"screen",	{Qscreen},	0,			0444,
};

#define	NBIT	(sizeof bitdir/sizeof(Dirtab))
#define	NINFO	257

void
bitreset(void)
{
	int i;
	GBitmap *bp;
	ulong r;

	bit.map = ialloc(conf.nbitmap * sizeof(GBitmap), 0);
	for(i=0,bp=bit.map; i<conf.nbitmap; i++,bp++){
		bp->ldepth = -1;
		bp->base = (ulong*)(bp+1);
	}
	bp--;
	bp->base = 0;
	bit.map[0] = gscreen;	/* bitmap 0 is screen */
	getcolor(0, &r, &r, &r);
	if(r == 0)
		flipping = 1;
	bit.free = bit.map+1;
	bit.lastid = -1;
	bit.lastfid = -1;
	bit.words = ialloc(conf.nbitbyte, 0);
	bit.nwords = conf.nbitbyte/sizeof(ulong);
	bit.wfree = bit.words;
	bit.font = ialloc(conf.nfont * sizeof(GFont), 0);
	bit.font[0] = *defont;
	for(i=1; i<conf.nfont; i++)
		bit.font[i].info = ialloc((NINFO+1)*sizeof(Fontchar), 0);
	Cursortocursor(&arrow);
}

void
bitinit(void)
{
	lock(&bit);
	unlock(&bit);
	if(gscreen.ldepth > 3)
		cursorback.ldepth = 0;
	else {
		cursorback.ldepth = gscreen.ldepth;
		cursorback.width = ((16 << gscreen.ldepth) + 31) >> 5;
	}
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
	if(c->qid.path != CHDIR)
		incref(&bit);
	return nc;
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
	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}else if(c->qid.path == Qbitblt){
		lock(&bit);
		if(bit.ref){
			unlock(&bit);
			error(Einuse);
		}
		bit.lastid = -1;
		bit.lastfid = -1;
		bit.rid = -1;
		bit.mid = -1;
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
	USED(c, name, omode, perm);
	error(Eperm);
}

void
bitremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
bitwstat(Chan *c, char *db)
{
	USED(c, db);
	error(Eperm);
}

void
bitclose(Chan *c)
{
	int i;
	GBitmap *bp;
	GFont *fp;

	if(c->qid.path!=CHDIR && (c->flag&COPEN)){
		lock(&bit);
		if(--bit.ref == 0){
			for(i=1,bp=&bit.map[1]; i<conf.nbitmap; i++,bp++)
				if(bp->ldepth >= 0)
					bitfree(bp);
			for(i=1,fp=&bit.font[1]; i<conf.nfont; i++,fp++)
				fp->bits = 0;
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
 * bit order. So far all machines have the same (good) order; when
 * that changes, these should switch on a variable set at init time.
 */
#define	U2K(x)	(x)
#define	K2U(x)	(x)

long
bitread(Chan *c, void *va, long n, ulong offset)
{
	uchar *p, *q;
	long miny, maxy, t, x, y;
	ulong l, nw, ws, rv, gv, bv;
	int off, j;
	Fontchar *i;
	GBitmap *src;

	if(c->qid.path & CHDIR)
		return devdirread(c, va, n, bitdir, NBIT, devgen);

	switch(c->qid.path){
	case Qmouse:
		/*
		 * mouse:
		 *	'm'		1
		 *	buttons		1
		 * 	point		8
		 */
		if(n < 10)
			error(Ebadblt);
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
			 * if count great enough, also
			 *	font info	3*12
			 *	fontchars	6*(defont->n+1)
			 */
			if(n < 18)
				error(Ebadblt);
			p[0] = 'I';
			p[1] = gscreen.ldepth;
			PLONG(p+2, gscreen.r.min.x);
			PLONG(p+6, gscreen.r.min.y);
			PLONG(p+10, gscreen.r.max.x);
			PLONG(p+14, gscreen.r.max.y);
			if(n >= 18+3*12+6*(defont->n+1)){
				p += 18;
				sprint((char*)p, "%11d %11d %11d ", defont->n,
					defont->height, defont->ascent);
				p += 3*12;
				for(i=defont->info,j=0; j<=defont->n; j++,i++,p+=6){
					PSHORT(p, i->x);
					p[2] = i->top;
					p[3] = i->bottom;
					p[4] = i->left;
					p[5] = i->width;
				}
				n = 18+3*12+6*(defont->n+1);
			}else
				n = 18;
			bit.init = 0;
			break;
		}
		if(bit.lastid > 0){
			/*
			 * allocate:
			 *	'A'		1
			 *	bitmap id	2
			 */
			if(n < 3)
				error(Ebadblt);
			p[0] = 'A';
			PSHORT(p+1, bit.lastid);
			bit.lastid = -1;
			n = 3;
			break;
		}
		if(bit.lastfid > 0){
			/*
			 * allocate font:
			 *	'K'		1
			 *	font id		2
			 */
			if(n < 3)
				error(Ebadblt);
			p[0] = 'K';
			PSHORT(p+1, bit.lastfid);
			bit.lastfid = -1;
			n = 3;
			break;
		}
		if(bit.mid >= 0){
			/*
			 * read colormap:
			 *	data		12*(2**bitmapdepth)
			 */
			l = (1<<bit.map[bit.mid].ldepth);
			nw = 1 << l;
			if(n < 12*nw)
				error(Ebadblt);
			for(j = 0; j < nw; j++){
				if(bit.mid == 0){
					getcolor(flipping? ~j : j, &rv, &gv, &bv);
				}else{
					rv = j;
					for(off = 32-l; off > 0; off -= l)
						rv = (rv << l) | j;
					gv = bv = rv;
				}
				PLONG(p, rv);
				PLONG(p+4, gv);
				PLONG(p+8, bv);
				p += 12;
			}
			bit.mid = -1;
			n = 12*nw;
			break;
		}
		if(bit.rid >= 0){
			/*
			 * read bitmap:
			 *	data		bytewidth*(maxy-miny)
			 */
			src = &bit.map[bit.rid];
			if(src->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			if(bit.rid == 0)
				off = 1;
			miny = bit.rminy;
			maxy = bit.rmaxy;
			if(miny>maxy || miny<src->r.min.y || maxy>src->r.max.y)
				error(Ebadblt);
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
				error(Ebadblt);
			if(off)
				cursoroff(1);
			n = 0;
			p = va;
			for(y=miny; y<maxy; y++){
				q = (uchar*)gaddr(src, Pt(src->r.min.x, y));
				q += (src->r.min.x&((sizeof(ulong))*ws-1))/ws;
				if(bit.rid == 0 && flipping)	/* flip bits */
					for(x=0; x<l; x++)
						*p++ = ~K2U(*q++);
				else
					for(x=0; x<l; x++)
						*p++ = K2U(*q++);
				n += l;
			}
			if(off)
				cursoron(1);
			bit.rid = -1;
			break;
		}
		error(Ebadblt);

	case Qscreen:
		if(offset==0){
			if(n < 5*12)
				error(Eio);
			sprint(va, "%11d %11d %11d %11d %11d ",
				gscreen.ldepth, gscreen.r.min.x,
				gscreen.r.min.y, gscreen.r.max.x,
				gscreen.r.max.y);
			n = 5*12;
			break;
		}
		ws = 1<<(3-gscreen.ldepth);	/* pixels per byte */
		l = (gscreen.r.max.x+ws-1)/ws - gscreen.r.min.x/ws;
		t = offset-5*12;
		miny = t/l;	/* unsigned computation */
		maxy = (t+n)/l;
		if(miny >= gscreen.r.max.y)
			return 0;
		if(maxy >= gscreen.r.max.y)
			maxy = gscreen.r.max.y;
		n = 0;
		p = va;
		for(y=miny; y<maxy; y++){
			q = (uchar*)gaddr(&gscreen, Pt(0, y));
			if(flipping)
				for(x=0; x<l; x++)
					*p++ = ~K2U(*q++);
			else
				for(x=0; x<l; x++)
					*p++ = K2U(*q++);
			n += l;
		}
		break;

	default:
		error(Egreg);
	}

	return n;
}


long
bitwrite(Chan *c, void *va, long n, ulong offset)
{
	uchar *p, *q;
	long m, v, miny, maxy, minx, maxx, t, x, y;
	ulong l, nw, ws, rv;
	int off, isoff, i, ok;
	Point pt, pt1, pt2;
	Rectangle rect;
	Cursor curs;
	Fcode fc;
	Fontchar *fcp;
	GBitmap *bp, *src, *dst;
	GFont *f;

	if(c->qid.path == CHDIR)
		error(Eisdir);

	if(c->qid.path != Qbitblt)
		error(Egreg);

	isoff = 0;
	if(waserror()){
		if(isoff)
			cursoron(1);
		nexterror();
	}
	p = va;
	m = n;
	while(m > 0)
		switch(*p){
		default:
			pprint("bitblt request 0x%x\n", *p);
			error(Ebadblt);

		case 'a':
			/*
			 * allocate:
			 *	'a'		1
			 *	ldepth		1
			 *	Rectangle	16
			 * next read returns allocated bitmap id
			 */
			if(m < 18)
				error(Ebadblt);
			v = *(p+1);
			if(v>3)	/* BUG */
				error(Ebadblt);
			ws = 1<<(5-v);	/* pixels per word */
			if(bit.free == 0)
				error(Enobitmap);
			rect.min.x = GLONG(p+2);
			rect.min.y = GLONG(p+6);
			rect.max.x = GLONG(p+10);
			rect.max.y = GLONG(p+14);
			if(Dx(rect) < 0 || Dy(rect) < 0)
				error(Ebadblt);
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
					error(Enobitstore);
			}
			bp = bit.free;
			bit.free = (GBitmap*)(bp->base);
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
				error(Ebadblt);
			fc = GSHORT(p+29) & 0xF;
			v = GSHORT(p+11);
			src = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || src->ldepth < 0)
				error(Ebadbitmap);
			off = 0;
			if(v == 0){
				if(flipping)
					fc = flipS[fc];
				off = 1;
			}
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth < 0)
				error(Ebadbitmap);
			if(v == 0){
				if(flipping)
					fc = flipD[fc];
				off = 1;
			}
			pt.x = GLONG(p+3);
			pt.y = GLONG(p+7);
			rect.min.x = GLONG(p+13);
			rect.min.y = GLONG(p+17);
			rect.max.x = GLONG(p+21);
			rect.max.y = GLONG(p+25);
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			ubitblt(dst, pt, src, rect, fc);
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
				if(!isoff){
					cursoroff(1);
					isoff = 1;
				}
				Cursortocursor(&arrow);
				m -= 1;
				p += 1;
				break;
			}
			if(m < 73)
				error(Ebadblt);
			curs.offset.x = GLONG(p+1);
			curs.offset.y = GLONG(p+5);
			memmove(curs.clr, p+9, 2*16);
			memmove(curs.set, p+41, 2*16);
			if(!isoff){
				cursoroff(1);
				isoff = 1;
			}
			Cursortocursor(&curs);
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
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			bitfree(dst);
			m -= 3;
			p += 3;
			break;

		case 'g':
			/*
			 * free font (free bitmap separately)
			 *	'g'		1
			 *	id		2
			 */
			if(m < 3)
				error(Ebadblt);
			v = GSHORT(p+1);
			f = &bit.font[v];
			if(v<0 || v>=conf.nfont || f->bits==0)
				error(Ebadfont);
			f->bits = 0;
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

		case 'k':
			/*
			 * allocate font
			 *	'k'		1
			 *	n		2
			 *	height		1
			 *	ascent		1
			 *	bitmap id	2
			 *	fontchars	6*(n+1)
			 * next read returns allocated font id
			 */
			if(m < 7)
				error(Ebadblt);
			v = GSHORT(p+1);
			if(v<0 || v>NINFO || m<7+6*(v+1))	/* BUG */
				error(Ebadblt);
			for(i=1; i<conf.nfont; i++)
				if(bit.font[i].bits == 0)
					goto fontfound;
			error(Enofont);
		fontfound:
			f = &bit.font[i];
			f->n = v;
			f->height = p[3];
			f->ascent = p[4];
			v = GSHORT(p+5);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			m -= 7;
			p += 7;
			fcp = f->info;
			for(i=0; i<=f->n; i++,fcp++){
				fcp->x = GSHORT(p);
				fcp->top = p[2];
				fcp->bottom = p[3];
				fcp->left = p[4];
				fcp->width = p[5];
				fcp->top = p[2];
				p += 6;
				m -= 6;
			}
			bit.lastfid = f - bit.font;
			f->bits = dst;
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
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			fc = GSHORT(p+20) & 0xF;
			if(v == 0){
				if(flipping)
					fc = flipD[fc];
				off = 1;
			}
			pt1.x = GLONG(p+3);
			pt1.y = GLONG(p+7);
			pt2.x = GLONG(p+11);
			pt2.y = GLONG(p+15);
			t = p[19];
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			gsegment(dst, pt1, pt2, t, fc);
			m -= 22;
			p += 22;
			break;

		case 'm':
			/*
			 * read colormap
			 *
			 *	'm'		1
			 *	id		2
			 */
			if(m < 3)
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			bit.mid = v;
			m -= 3;
			p += 3;
			break;

		case 'p':
			/*
			 * point
			 *
			 *	'p'		1
			 *	id		2
			 *	pt		8
			 *	value		1
			 *	code		2
			 */
			if(m < 14)
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			fc = GSHORT(p+12) & 0xF;
			if(v == 0){
				if(flipping)
					fc = flipD[fc];
				off = 1;
			}
			pt1.x = GLONG(p+3);
			pt1.y = GLONG(p+7);
			t = p[11];
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			gpoint(dst, pt1, t, fc);
			m -= 14;
			p += 14;
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
				error(Ebadblt);
			v = GSHORT(p+1);
			src = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || src->ldepth<0)
				error(Ebadbitmap);
			miny = GLONG(p+3);
			maxy = GLONG(p+7);
			if(miny>maxy || miny<src->r.min.y || maxy>src->r.max.y)
				error(Ebadblt);
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
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			fc = GSHORT(p+13) & 0xF;
			if(v == 0){
				if(flipping)
					fc = flipD[fc];
				off = 1;
			}
			pt.x = GLONG(p+3);
			pt.y = GLONG(p+7);
			v = GSHORT(p+11);
			f = &bit.font[v];
			if(v<0 || v>=conf.nfont || f->bits==0 || f->bits->ldepth<0)
				error(Ebadblt);
			p += 15;
			m -= 15;
			q = memchr(p, 0, m);
			if(q == 0)
				error(Ebadblt);
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			gstring(dst, pt, f, (char*)p, fc);
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
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			fc = GSHORT(p+21) & 0xF;
			if(v == 0){
				if(flipping)
					fc = flipD[fc];
				off = 1;
			}
			rect.min.x = GLONG(p+3);
			rect.min.y = GLONG(p+7);
			rect.max.x = GLONG(p+11);
			rect.max.y = GLONG(p+15);
			v = GSHORT(p+19);
			src = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || src->ldepth<0)
				error(Ebadbitmap);
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			gtexture(dst, rect, src, fc);
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
				error(Ebadblt);
			v = GSHORT(p+1);
			dst = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || dst->ldepth<0)
				error(Ebadbitmap);
			off = 0;
			if(v == 0)
				off = 1;
			miny = GLONG(p+3);
			maxy = GLONG(p+7);
			if(miny>maxy || miny<dst->r.min.y || maxy>dst->r.max.y)
				error(Ebadblt);
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
				error(Ebadblt);
			if(off && !isoff){
				cursoroff(1);
				isoff = 1;
			}
			for(y=miny; y<maxy; y++){
				q = (uchar*)gaddr(dst, Pt(dst->r.min.x, y));
				q += (dst->r.min.x&((sizeof(ulong))*ws-1))/ws;
				if(v == 0 && flipping)	/* flip bits */
					for(x=0; x<l; x++)
						*q++ = ~U2K(*p++);
				else
					for(x=0; x<l; x++)
						*q++ = U2K(*p++);
				m -= l;
			}
			break;

		case 'x':
			/*
			 * cursorset
			 *
			 *	'x'		1
			 *	pt		8
			 */
			if(m < 9)
				error(Ebadblt);
			pt1.x = GLONG(p+1);
			pt1.y = GLONG(p+5);
/*			if(!eqpt(mouse.xy, pt1))*/{
				mouse.xy = pt1;
				mouse.track = 1;
				mouseclock();
			}
			m -= 9;
			p += 9;
			break;

		case 'z':
			/*
			 * write the colormap
			 *
			 *	'z'		1
			 *	id		2
			 *	map		12*(2**bitmapdepth)
			 */
			if(m < 3)
				error(Ebadblt);
			v = GSHORT(p+1);
			if(v != 0)
				error(Ebadbitmap);
			m -= 3;
			p += 3;
			nw = 1 << (1 << bit.map[v].ldepth);
			if(m < 12*nw)
				error(Ebadblt);
			ok = 1;
			for(i = 0; i < nw; i++){
				ok &= setcolor(i, GLONG(p), GLONG(p+4), GLONG(p+8));
				p += 12;
				m -= 12;
			}
			if(!ok){
				/* assume monochrome: possibly change flipping */
				l = GLONG(p-12);
				getcolor(nw-1, &rv, &rv, &rv);
				flipping = (l != rv);
			}
			break;
		}

	poperror();
	if(isoff)
		cursoron(1);
	return n;
}

void
bitfree(GBitmap *bp)
{
	bp->base[-1] = 0;
	bp->ldepth = -1;
	bp->base = (ulong*)bit.free;
	bit.free = bp;
}

QLock	bitlock;

GBitmap *
id2bit(int k)
{
	GBitmap *bp;
	bp = &bit.map[k];
	if(k<0 || k>=conf.nbitmap || bp->ldepth < 0)
		error(Ebadbitmap);
	return bp;
}

void
bitcompact(void)
{
	ulong *p1, *p2;

	qlock(&bitlock);
	p1 = p2 = bit.words;
	while(p2 < bit.wfree){
		if(p2[1] == 0){
			p2 += 2 + p2[0];
			continue;
		}
		if(p1 != p2){
			memmove(p1, p2, (2+p2[0])*sizeof(ulong));
			((GBitmap*)p1[1])->base = p1+2;
		}
		p2 += 2 + p1[0];
		p1 += 2 + p1[0];
	}
	bit.wfree = p1;
	qunlock(&bitlock);
}

void
Cursortocursor(Cursor *c)
{
	int i;
	uchar *p;

	lock(&cursor);
	memmove(&cursor, c, sizeof(Cursor));
	for(i=0; i<16; i++){
		p = (uchar*)&setbits[i];
		*p = c->set[2*i];
		*(p+1) = c->set[2*i+1];
		p = (uchar*)&clrbits[i];
		*p = c->clr[2*i];
		*(p+1) = c->clr[2*i+1];
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
		kbitblt(&cursorback, Pt(0, 0), &gscreen, cursor.r, S);
		kbitblt(&gscreen, add(mouse.xy, cursor.offset),
			&clr, Rect(0, 0, 16, 16), flipping? flipD[D&~S] : D&~S);
		kbitblt(&gscreen, add(mouse.xy, cursor.offset),
			&set, Rect(0, 0, 16, 16), flipping? flipD[S|D] : S|D);
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
		kbitblt(&gscreen, cursor.r.min, &cursorback, Rect(0, 0, 16, 16), S);
	if(dolock)
		unlock(&cursor);
}

void
mousedelta(int b, int dx, int dy)	/* called at higher priority */
{
	mouse.dx += dx;
	mouse.dy += dy;
	mouse.newbuttons = b;
	mouse.track = 1;
}

void
mousebuttons(int b)	/* called at higher priority */
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
mouseupdate(int dolock)
{
	int x, y;

	if(!mouse.track || (dolock && !canlock(&cursor)))
		return;

	x = mouse.xy.x + mouse.dx;
	if(x < gscreen.r.min.x)
		x = gscreen.r.min.x;
	if(x >= gscreen.r.max.x)
		x = gscreen.r.max.x;
	y = mouse.xy.y + mouse.dy;
	if(y < gscreen.r.min.y)
		y = gscreen.r.min.y;
	if(y >= gscreen.r.max.y)
		y = gscreen.r.max.y;
	cursoroff(0);
	mouse.xy = Pt(x, y);
	cursoron(0);
	mouse.dx = 0;
	mouse.dy = 0;
	mouse.clock = 0;
	mouse.track = 0;
	mouse.buttons = mouse.newbuttons;
	mouse.changed = 1;

	if(dolock){
		unlock(&cursor);
		wakeup(&mouse.r);
	}
}

int
mouseputc(IOQ *q, int c)
{
	static short msg[5];
	static int nb;
	static uchar b[] = {0, 4, 2, 6, 1, 5, 3, 7};

	USED(q);
	if((c&0xF0) == 0x80)
		nb=0;
	msg[nb] = c;
	if(c & 0x80)
		msg[nb] |= 0xFF00;	/* sign extend */
	if(++nb == 5){
		mouse.newbuttons = b[(msg[0]&7)^7];
		mouse.dx = msg[1]+msg[3];
		mouse.dy = -(msg[2]+msg[4]);
		mouse.track = 1;
		mouseclock();
		nb = 0;
	}
}

int
mousechanged(void *m)
{
	USED(m);
	return mouse.changed;
}
