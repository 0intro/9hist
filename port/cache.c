#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"

Image	fscache;

typedef Extent Extent;
struct Extent
{
	int	bid;
	ulong	start;
	int	len;
	Extent	*next;
};

typedef struct Mntcache Mntcache;
struct Mntcache
{
	Qid;
	int	dev;
	int	type;
	Qlock;
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

Mntcache*
clook(Chan *c)
{
	int h;

	h = c->qid.path%NHASH;

	lock(&cache);
	for(m = cache.hash[h]; m; m = m->hash) {
		if(m->path == c->path) {
			qlock(m);
			if(m->path == c->qid.path)
			if(m->dev  == c->dev)
			if(m->type == c->type) {
				unlock(&cache);
				return m;
			}
			qunlock(m);
		}
	}
	unlock(&cache);
	return 0;
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
		n = e->list;
		free(e);
	}
}

void
ctail(Mntcache *m)
{
	lock(&cache);

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

	unlock(&cache);
}

void
copen(Chan *c)
{
	Mntcache *m, *f, **l;

	m = clook(c);
	if(m != 0) {
		/* File was updated */
		if(m->vers != c->vers)
			cnodata(m);

		ctail(m);
		qunlock(m);
		return;
	}

	/* LRU the cache headers */
	m = cache.head;
	qlock(m);
	lock(cache);
	l = &cache.hash[m->qid.path%NHASH];
	for(f = *l; f; f = f->next) {
		if(f == m) {
			*l = f->next;
			break;
		}
		l = &f->next;
	}
	l = &cache.hash[c->qid.path%NHASH];
	m->hash = *l;
	*l = m;
	unlock(cache);
	m->qid = c->qid;
	m->dev = c->dev;
	m->type = c->type;
	cnodata(m);
	ctail(m);
	c->mcp = m;
	qunlock(m);
}

static int
cdev(Mntcache *m, Chan *c)
{
	if(m->path != c->path)
		return 0;
	if(m->vers != c->vers)
		return 0;
	if(m->dev != c->dev)
		return 0;
	if(m->type != c->type)
		return 0;
	return 1;
}

int
cread(Chan *c, uchar *buf, int len, long offset)
{
	KMap *k;
	Page *p;
	Mntcache *m;
	Extent *e, **l;
	int o, l, total;

	m = c->mcp;
	if(m == 0)
		return 0;
	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return 0;
	}

	end = offset+len;
	l = &m->list;
	for(e = *l; e; e = e->next) {
		if(e->start >= offset && e->start+e->len < end)
			break;
		l = &e->next;
	}

	if(e == 0) {
		qunlock(m);
		return 0;
	}

	total = 0;
	while(len) {
		p = lookpage(&fscache, e->bid);
		if(p == 0) {
			*l = e->next;
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

		p = (uchar*)VA(k) + o;
		memset(buf, p, l);
		kunmap(k);
		putpage(p);

		buf += l;
		len -= l;
		offset += l;
		total += l;
		l = &e->next;
		e = e->next;
		if(e == 0 || e->start != offset)
			break;
	}
	qunlock(m)
	return total;
}

void
cwrite(Chan *c, uchar *buf, int len, long offset)
{
	Extent *e;
	Mntcache *m;

	if(offset > MAXCACHE)
		return;

	m = c->mcp;
	if(m == 0)
		return;
	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return;
	}

	if(m->list == 0) {
		e = malloc(sizeof(Extent));
		if(e == 0)
			return;
		e->start = offset;
		l = len;
		if(l > BY2PG)
			l = BY2PG;
		p = auxpage();
		if(p == 0)
			return;
		e->bid = incref(&cache);
		p->daddr = e->bid;
		cachepage(p, fscache);
		k = kmap(p);
		
		kunmap(p);
	}
}
