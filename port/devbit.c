#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"gnot.h"

extern	Bitmap	screen;

struct{
	Ref;
	int	bltuse;
	QLock	blt;		/* a group of bitblts in a single write is atomic */
}bit;

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
		/*
		 * Always open #b/bitblt first
		 */
		lock(&bit);
		if((c->qid==Qbitblt && bit.bltuse)
		|| (c->qid!=Qbitblt && !bit.bltuse)){
			unlock(&bit);
			error(0, Einuse);
		}
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
	if(c->qid != CHDIR){
		lock(&bit);
		if(--bit.ref == 0)
			bit.bltuse = 0;
		unlock(&bit);
	}
}

long
bitread(Chan *c, void *va, long n)
{
	if(c->qid & CHDIR)
		return devdirread(c, va, n, bitdir, NBIT, devgen);

	error(0, Egreg);
}

#define	SHORT(p)	(((p)[0]<<0) | ((p)[1]<<8))
#define	LONG(p)		((SHORT(p)<<0) | (SHORT(p+2)<<16))

long
bitwrite(Chan *c, void *va, long n)
{
	uchar *p;
	long m;
	long v;
	Point pt;
	Rectangle rect;
	Bitmap *src, *dst;

	if(c->qid == CHDIR)
		error(0, Eisdir);

	p = va;
	m = n;
	switch(c->qid){
	case Qbitblt:
		qlock(&bit.blt);
		if(waserror()){
			qunlock(&bit.blt);
			nexterror();
		}
		while(m > 0)
			switch(*p){
			case 'b':
				if(m < 31)
					error(0, Ebadblt);
				v = SHORT(p+1);
				if(v != 0)		/* BUG */
					error(0, Ebadblt);
				dst = &screen;
				pt.x = LONG(p+3);
				pt.y = LONG(p+7);
				v = SHORT(p+11);
				if(v != 0)		/* BUG */
					error(0, Ebadblt);
				src = &screen;
				rect.min.x = LONG(p+13);
				rect.min.y = LONG(p+17);
				rect.max.x = LONG(p+21);
				rect.max.y = LONG(p+25);
				v = SHORT(p+29);
				bitblt(dst, pt, src, rect, v);
				m -= 31;
				p += 31;
				break;
			default:
				error(0, Ebadblt);
			}
		qunlock(&bit.blt);
		break;

	default:
		error(0, Egreg);
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
