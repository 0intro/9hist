#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"gnot.h"

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
	QLock	blt;		/* a group of bitblts in a single write is atomic */
	Bitmap	*map;		/* arena */
	Bitmap	*free;		/* free list */
	ulong	*words;		/* storage */
	ulong	nwords;		/* total in arena */
	ulong	*wfree;		/* pointer to next free word */
	int	lastid;		/* last allocated bitmap id */
}bit;

#define	FREE	0x80000000
void	bitcompact(void);
void	bitfree(Bitmap*);
extern	Bitmap	screen;

enum{
	Qdir,
	Qbitblt,
};

Dirtab bitdir[]={
	"bitblt",	Qbitblt,	0,			0600,
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
}

void
bitinit(void)
{
	lock(&bit);
	bit.bltuse = 0;
	unlock(&bit);
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
		lock(&bit);			/* FREE ALL THE BITMAPS: BUG */
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

long
bitread(Chan *c, void *va, long n)
{
	uchar *p;

	if(c->qid & CHDIR)
		return devdirread(c, va, n, bitdir, NBIT, devgen);

	if(c->qid != Qbitblt)
		error(0, Egreg);
	p = va;
	qlock(&bit.blt);
	if(waserror()){
		qunlock(&bit.blt);
		nexterror();
	}
	/*
	 * Fuss about and figure out what to say.
	 */
	if(bit.lastid > 0){
		if(n < 3)
			error(0, Ebadblt);
		p[0] = 'A';
		p[1] = bit.lastid;
		p[2] = bit.lastid>>8;
		bit.lastid = -1;
		n = 3;
		goto done;
	}
	error(0, Ebadblt);

    done:
	qunlock(&bit.blt);
	return n;
}


long
bitwrite(Chan *c, void *va, long n)
{
	uchar *p;
	long m;
	long v;
	ulong l, nw, ws;
	Point pt;
	Rectangle rect;
	Bitmap *bp, *src, *dst;

	if(c->qid == CHDIR)
		error(0, Eisdir);

	if(c->qid != Qbitblt)
		error(0, Egreg);

	p = va;
	m = n;
	qlock(&bit.blt);
	if(waserror()){
		qunlock(&bit.blt);
		nexterror();
	}
	while(m > 0)
		switch(*p){
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
				long t;
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
			bp->rect = rect;
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
				error(0, Ebadblt);
			pt.x = GLONG(p+3);
			pt.y = GLONG(p+7);
			v = GSHORT(p+11);
			src = &bit.map[v];
			if(v<0 || v>=conf.nbitmap || src->ldepth < 0)
				error(0, Ebadblt);
			rect.min.x = GLONG(p+13);
			rect.min.y = GLONG(p+17);
			rect.max.x = GLONG(p+21);
			rect.max.y = GLONG(p+25);
			v = GSHORT(p+29);
			bitblt(dst, pt, src, rect, v);
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
			if(v >= conf.nbitmap || dst->ldepth<0)
				error(0, Ebadblt);
			bitfree(dst);
			m -= 3;
			p += 3;
			break;
		}

	qunlock(&bit.blt);
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
