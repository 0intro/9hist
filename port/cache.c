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
	Lock;
	int		pgno;
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
cprint(Chan *c, Mntcache *m, char *s)
{
	ulong o;
	int nb, ct;
	Extent *e;
	char buf[128];
return;
	ptpath(c->path, buf, sizeof(buf));
	nb = 0;
	ct = 1;
	if(m->list)
		o = m->list->start;
	for(e = m->list; e; e = e->next) {
		nb += e->len;
		if(o != e->start)
			ct = 0;
		o = e->start+e->len;
	}
	pprint("%s: 0x%lux.0x%lux %d %d %s (%d %c)\n",
	s, m->path, m->vers, m->type, m->dev, buf, nb, ct ? 'C' : 'N');

	for(e = m->list; e; e = e->next) {
		if(0) pprint("\t%4d %5d %4d %lux\n",
			e->bid, e->start, e->len, e->cache);
	}
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
		cache.head = m;
		cache.tail = m;
		m->prev = 0;
		m->next = 0;
	}
}

void
copen(Chan *c)
{
	int h;
	Extent *e, *next;
	Mntcache *m, *f, **l;

	if(c->qid.path&CHDIR)
		return;

	h = c->qid.path%NHASH;
	lock(&cache);
	for(m = cache.hash[h]; m; m = m->hash) {
		if(m->path == c->qid.path)
		if(m->dev == c->dev && m->type == c->type) {
			c->mcp = m;
			ctail(m);
			unlock(&cache);

			/* File was updated, invalidate cache */
			if(m->vers != c->qid.vers) {
				m->vers = c->qid.vers;
				qlock(m);
				cnodata(m);
				qunlock(m);
cprint(c, m, "open mod");
			}
else
cprint(c, m, "open cached");
			return;
		}
	}

	/* LRU the cache headers */
	m = cache.head;
	l = &cache.hash[m->path%NHASH];
	for(f = *l; f; f = f->hash) {
		if(f == m) {
			*l = m->hash;
			break;
		}
		l = &f->hash;
	}

	m->Qid = c->qid;
	m->dev = c->dev;
	m->type = c->type;

	l = &cache.hash[h];
	m->hash = *l;
	*l = m;
	ctail(m);

	c->mcp = m;
	e = m->list;
	m->list = 0;
	unlock(&cache);

	while(e) {
		next = e->next;
		free(e);
		e = next;
	}
cprint(c, m, "open new");
}

static int
cdev(Mntcache *m, Chan *c)
{
	if(m->path != c->qid.path)
		return 0;
	if(m->dev != c->dev)
		return 0;
	if(m->type != c->type)
		return 0;
	if(m->vers != c->qid.vers)
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
	int o, l, total;

	m = c->mcp;
	if(m == 0)
		return 0;

	qlock(m);
	if(cdev(m, c) == 0) {
		qunlock(m);
		return 0;
	}

	t = &m->list;
	for(e = *t; e; e = e->next) {
		if(offset >= e->start && offset < e->start+e->len)
			break;
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

		lock(&cache);
		e->bid = cache.pgno;
		cache.pgno += BY2PG;
		unlock(&cache);

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
cupdate(Chan *c, uchar *buf, int len, ulong offset)
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
	c->qid.vers++;

	p = 0;
	for(f = m->list; f; f = f->next) {
		if(f->start >= offset)
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
		/* Pack sequential write if there is space */
		if(ee == offset && p->len < BY2PG) {
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
}
