#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"

Image	fscache;

enum
{
	NHASH		= 128,
	MAXCACHE	= 1024*1024,
	NFILE		= 4096,
};

typedef struct Extent Extent;
struct Extent
{
	int	bid;
	ulong	start;
	int	len;
	Page	*cache;
	Extent	*next;
};

typedef struct Mntcache Mntcache;
struct Mntcache
{
	Qid;
	int	dev;
	int	type;
	QLock;
	Extent	 *list;
	Mntcache *hash;
	Mntcache *prev;
	Mntcache *next;
};

typedef struct Cache Cache;
struct Cache
{
	Ref;
	Mntcache	*head;
	Mntcache	*tail;
	Mntcache	*hash[NHASH];
};
Cache cache;

void
cinit(void)
{
	int i;
	Mntcache *m;

	cache.ref = 1;
	cache.head = xalloc(sizeof(Mntcache)*NFILE);
	m = cache.head;
	
	for(i = 0; i < NFILE-1; i++) {
		m->next = m+1;
		m->prev = m-1;
		m++;
	}

	cache.tail = m;
	cache.tail->next = 0;
	cache.head->prev = 0;
}

void
cprint(Mntcache *m, char *s)
{
	Extent *e;

	print("%s: 0x%lux.0x%lux %d %d\n",
			s, m->path, m->vers, m->type, m->dev);

	for(e = m->list; e; e = e->next)
		print("\t%4d %5d %4d %lux\n",
			e->bid, e->start, e->len, e->cache);
}

Page*
cpage(Extent *e)
{
	/* Easy consistency check */
	if(e->cache->daddr != e->bid)
		return 0;

	return lookpage(&fscache, e->bid);
}

void
cnodata(Mntcache *m)
{
	Extent *e, *n;

	/*
	 * Invalidate all extent data
	 * Image lru will waste the pages
	 */
	for(e = m->list; e; e = n) {
		n = e->next;
		free(e);
	}
	m->list = 0;
}

void
ctail(Mntcache *m)
{
	/* Unlink and send to the tail */
	if(m->prev) 
		m->prev->next = m->next;
	else
		cache.head = m->next;
	if(m->next)
		m->next->prev = m->prev;
	else
		cache.tail = m->prev;

	if(cache.tail) {
		m->prev = cache.tail;
		cache.tail->next = m;
		m->next = 0;
		cache.tail = m;
	}
	else {
		cache.head = cache.tail = m;
		m->prev = m->next = 0;
	}
}

void
copen(Chan *c)
{
	Mntcache *m, *f, **l;

	if(c->qid.path & CHDIR)
		return;

	lock(&cache);
	for(m = cache.hash[c->qid.path%NHASH]; m; m = m->hash) {
		if(m->path == c->qid.path)
		if(m->dev == c->dev && m->type == c->type) {
			c->mcp = m;
			ctail(m);
			unlock(&cache);

			/* File was updated, invalidate cache */
			if(m->vers != c->qid.vers) {
				qlock(m);
				cnodata(m);
				m->vers = c->qid.vers;
				qunlock(m);
			}
			return;
		}
	}

	/* LRU the cache headers */
	m = cache.head;
	l = &cache.hash[m->path%NHASH];
	for(f = *l; f; f = f->hash) {
		if(f == m) {
			*l = f->hash;
			break;
		}
		l = &f->hash;
	}
	l = &cache.hash[c->qid.path%NHASH];
	m->hash = *l;
	*l = m;
	ctail(m);

	m->Qid = c->qid;
	m->dev = c->dev;
	m->type = c->type;
	c->mcp = m;
	unlock(&cache);

	qlock(m);
	cnodata(m);
	qunlock(m);
}

static int
cdev(Mntcache *m, Chan *c)
{
	if(m->path != c->qid.path)
		return 0;
	if(m->vers != c->qid.vers)
		return 0;
	if(m->dev != c->dev)
		return 0;
	if(m->type != c->type)
		return 0;
	return 1;
}

int
cread(Chan *c, uchar *buf, int len, ulong offset)
{
	KMap *k;
	Page *p;
	Mntcache *m;
	Extent *e, **t;
	int o, l, total, end;

	if(c->qid.path & CHDIR)
		return 0;

	m = c->mcp;
	if(m == 0)
		return 0;

	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return 0;
	}

	end = offset+len;
	t = &m->list;
	for(e = *t; e; e = e->next) {
		if(offset > e->start && offset < e->start+e->len)
			break;
		if(offset < e->start) {
			qunlock(m);
			return 0;
		}	
		t = &e->next;
	}

	if(e == 0) {
		qunlock(m);
		return 0;
	}

	total = 0;
	while(len) {
		p = cpage(e);
		if(p == 0) {
			*t = e->next;
			free(e);
			qunlock(m);
			return total;
		}

		k = kmap(p);
		if(waserror()) {
			kunmap(k);
			putpage(p);
			qunlock(m);
			nexterror();
		}

		o = offset - e->start;
		l = len;
		if(l > e->len-o)
			l = e->len-o;

		memmove(buf, (uchar*)VA(k) + o, l);

		poperror();
		kunmap(k);
		putpage(p);

		buf += l;
		len -= l;
		offset += l;
		total += l;
		t = &e->next;
		e = e->next;
		if(e == 0 || e->start != offset)
			break;
	}
	qunlock(m);
	return total;
}

Extent*
cchain(uchar *buf, ulong offset, int len, Extent **tail)
{
	int l;
	Page *p;
	KMap *k;
	Extent *e, *start, **t;

	start = 0;
	*tail = 0;
	t = &start;
	while(len) {
		e = malloc(sizeof(Extent));
		if(e == 0)
			break;

		p = auxpage();
		if(p == 0) {
			free(e);
			break;
		}
		l = len;
		if(l > BY2PG)
			l = BY2PG;

		e->cache = p;
		e->start = offset;
		e->len = l;
		e->bid = incref(&cache);
		p->daddr = e->bid;
		k = kmap(p);
		memmove((void*)VA(k), buf, l);
		kunmap(k);

		cachepage(p, &fscache);
		putpage(p);

		buf += l;
		offset += l;
		len -= l;

		*t = e;
		*tail = e;
		t = &e->next;
	}

	return start;
}

int
cpgmove(Extent *e, uchar *buf, int boff, int len)
{
	Page *p;
	KMap *k;

	p = cpage(e);
	if(p == 0)
		return 0;

	k = kmap(p);
	memmove((uchar*)VA(k)+boff, buf, len);
	kunmap(k);
	putpage(p);

	return 1;
}

void
cxupdate(Chan *c, uchar *buf, int len, ulong offset)
{
	Mntcache *m;
	Extent *tail;
	Extent *e, *f, *p;
	int o, ee, eblock;

	if(offset > MAXCACHE || len == 0)
		return;

	m = c->mcp;
	if(m == 0)
		return;
	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return;
	}

	/*
	 * Find the insertion point
	 */
	p = 0;
	for(f = m->list; f; f = f->next) {
		if(f->start >= offset)
			break;
		p = f;
	}

	if(p == 0) {		/* at the head */
		eblock = offset+len;
		/* trim if there is a successor */
		if(f != 0 && eblock >= f->start) {
			len -= (eblock - f->start);
			if(len <= 0) {
				qunlock(m);
				return;
			}
		}
		e = cchain(buf, offset, len, &tail);
		if(e != 0) {
			m->list = e;
			if(tail != 0)
				tail->next = f;
		}
		qunlock(m);
		return;
	}

	/* trim to the predecessor */
	ee = p->start+p->len;
	if(offset < ee) {
		o = ee - offset;
		len -= o;
		if(len <= 0) {
			qunlock(m);
			return;
		}
		buf += o;
		offset += o;
	}

	/* try and pack data into the predecessor */
	if(offset == ee && p->len < BY2PG) {
		o = len;
		if(o > BY2PG - p->len)
			o = BY2PG - p->len;
		if(cpgmove(p, buf, p->len, o)) {
			p->len += o;
			buf += o;
			len -= o;
			offset += o;
			if(len <= 0) {
				qunlock(m);
				return;
			}
		}
	}

	/* append to extent list */
	if(f == 0) {
		p->next = cchain(buf, offset, len, &tail);
		qunlock(m);
		return;
	}

	/* trim data against successor */
	eblock = offset+len;
	if(eblock > f->start) {
		o = eblock - f->start;
		len -= o;
		if(len <= 0) {
			qunlock(m);
			return;
		}
	}

	/* insert a middle block */
	p->next = cchain(buf, offset, len, &tail);
	if(p->next == 0)
		p->next = f;
	else
		tail->next = f;

	qunlock(m);
}

void
cupdate(Chan *c, uchar *buf, int len, ulong offset)
{
	cxupdate(c, buf, len, offset);
	cprint(c->mcp, "cupdate");
}

void
cwrite(Chan* c, uchar *buf, int len, ulong offset)
{
	int o;
	Mntcache *m;
	ulong eblock, ee;
	Extent *p, *f, *e, *tail;

	if(offset > MAXCACHE || len == 0)
		return;

	m = c->mcp;
	if(m == 0)
		return;

	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return;
	}

	m->vers++;

	p = 0;
	for(f = m->list; f; f = f->next) {
		if(offset >= f->start)
			break;
		p = f;		
	}

	if(p != 0) {
		ee = p->start+p->len;
		if(ee > offset) {
			o = ee - offset;
			p->len -= o;
			if(p->len == 0)
				panic("del empty extent");
		}
	}

	eblock = offset+len;
	/* free the overlap - its a rare case */
	while(f && f->start < eblock) {
		e = f->next;
		free(f);
		f = e;
	}

	e = cchain(buf, offset, len, &tail);
	if(e != 0) {
		if(p == 0)
			m->list = e;
		else
			p->next = e;
		if(tail != 0)
			tail->next = f;
	}
	qunlock(m);
cprint(m, "cwrite");
}
