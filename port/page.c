#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#define	pghash(daddr)	palloc.hash[(daddr>>PGSHIFT)&(PGHSIZE-1)]

static	Lock pglock;
struct	Palloc palloc;

void
pageinit(void)
{
	int color;
	Page *p;
	ulong np, vm, pm;

	np = palloc.np0+palloc.np1;
	palloc.head = xalloc(np*sizeof(Page));
	if(palloc.head == 0)
		panic("pageinit");

	color = 0;
	p = palloc.head;
	while(palloc.np0 > 0) {
		p->prev = p-1;
		p->next = p+1;
		p->pa = palloc.p0;
		p->color = color;
		palloc.freecount++;
		color = (color+1)%NCOLOR;
		palloc.p0 += BY2PG;
		palloc.np0--;
		p++;
	}
	while(palloc.np1 > 0) {
		p->prev = p-1;
		p->next = p+1;
		p->pa = palloc.p1;
		p->color = color;
		palloc.freecount++;
		color = (color+1)%NCOLOR;
		palloc.p1 += BY2PG;
		palloc.np1--;
		p++;
	}
	palloc.tail = p - 1;
	palloc.head->prev = 0;
	palloc.tail->next = 0;

	palloc.user = p - palloc.head;
	pm = palloc.user*BY2PG/1024;
	vm = pm + (conf.nswap*BY2PG)/1024;

	/* Pageing numbers */
	swapalloc.highwater = (palloc.user*5)/100;
	swapalloc.headroom = swapalloc.highwater + (swapalloc.highwater/4);

	print("%lud free pages\n", palloc.user);
	print("%dK bytes\n", pm);
	print("%dK swap\n", vm);
}

Page*
newpage(int clear, Segment **s, ulong va)
{
	Page *p;
	KMap *k;
	uchar ct;
	int i, hw, dontalloc, color;


	lock(&palloc);
retry:
	color = getpgcolor(va);
	hw = swapalloc.highwater;
	for(;;) {
		if(palloc.freecount > hw)
			break;
		if(up->kp && palloc.freecount > 0)
			break;

		unlock(&palloc);
		dontalloc = 0;
		if(s && *s) {
			qunlock(&((*s)->lk));
			*s = 0;
			dontalloc = 1;
		}
		qlock(&palloc.pwait);	/* Hold memory requesters here */

		while(waserror())	/* Ignore interrupts */
			;

		kickpager();
		tsleep(&palloc.r, ispages, 0, 1000);

		poperror();

		qunlock(&palloc.pwait);

		/*
		 * If called from fault and we lost the segment from
		 * underneath don't waste time allocating and freeing
		 * a page. Fault will call newpage again when it has
		 * reacquired the segment locks
		 */
		if(dontalloc)
			return 0;

		lock(&palloc);
	}

	/* First try for our colour */
	for(p = palloc.head; p; p = p->next)
		if(p->color == color)
			break;

	ct = PG_NOFLUSH;
	if(p == 0) {
		p = palloc.head;
		p->color = color;
		ct = PG_NEWCOL;
	}

	if(p->prev)
		p->prev->next = p->next;
	else
		palloc.head = p->next;

	if(p->next)
		p->next->prev = p->prev;
	else
		palloc.tail = p->prev;

	palloc.freecount--;
	unlock(&palloc);

	lock(p);
	if(p->ref != 0) {	/* lookpage has priority on steal */
		unlock(p);
		print("stolen\n");
		lock(&palloc);
		palloc.freecount++;
		goto retry;
	}

	uncachepage(p);
	p->ref++;
	p->va = va;
	p->modref = 0;
	for(i = 0; i < MAXMACH; i++)
		p->cachectl[i] = ct;
	unlock(p);

	if(clear) {
		k = kmap(p);
		memset((void*)VA(k), 0, BY2PG);
		kunmap(k);
	}

	return p;
}

int
ispages(void*)
{
	return palloc.freecount >= swapalloc.highwater;
}

void
putpage(Page *p)
{
	if(onswap(p)) {
		putswap(p);
		return;
	}

	lock(p);
	if(--p->ref > 0) {
		unlock(p);
		return;
	}

	lock(&palloc);
	if(p->image && p->image != &swapimage) {
		if(palloc.tail) {
			p->prev = palloc.tail;
			palloc.tail->next = p;
		}
		else {
			palloc.head = p;
			p->prev = 0;
		}
		palloc.tail = p;
		p->next = 0;
	}
	else {
		if(palloc.head) {
			p->next = palloc.head;
			palloc.head->prev = p;
		}
		else {
			palloc.tail = p;
			p->next = 0;
		}
		palloc.head = p;
		p->prev = 0;
	}

	palloc.freecount++;
	if(palloc.r.p != 0)
		wakeup(&palloc.r);

	unlock(&palloc);
	unlock(p);
}

Page*
auxpage()
{
	Page *p;

	lock(&palloc);
	p = palloc.head;
	if(palloc.freecount < swapalloc.highwater) {
		unlock(&palloc);
		return 0;
	}
	p->next->prev = 0;
	palloc.head = p->next;
	palloc.freecount--;
	unlock(&palloc);

	lock(p);
	if(p->ref != 0) {		/* Stolen by lookpage */
		unlock(p);
		return 0;
	}
	p->ref++;
	uncachepage(p);
	unlock(p);
	return p;
}

void
duppage(Page *p)				/* Always call with p locked */
{
	Page *np;
	int color;

	/* No dup for swap/cache pages */
	if(p->image->notext)
		return;

	lock(&palloc);
	/* No freelist cache when memory is very low */
	if(palloc.freecount < swapalloc.highwater) {
		unlock(&palloc);
		uncachepage(p);
		return;
	}

	color = getpgcolor(p->va);
	for(np = palloc.head; np; np = np->next)
		if(np->color == color)
			break;

	/* No page of the correct color */
	if(np == 0) {
		unlock(&palloc);
		uncachepage(p);
		return;
	}

	if(np->prev)
		np->prev->next = np->next;
	else
		palloc.head = np->next;

	if(np->next)
		np->next->prev = np->prev;
	else
		palloc.tail = np->prev;

	/* Link back onto tail to give us lru in the free list */
	if(palloc.tail) {
		np->prev = palloc.tail;
		palloc.tail->next = np;
		np->next = 0;
		palloc.tail = np;
	}
	else {
		palloc.head = palloc.tail = np;
		np->prev = np->next = 0;
	}

	unlock(&palloc);

	lock(np);				/* Cache the new version */
	if(np->ref != 0) {			/* Stolen by lookpage */
		uncachepage(p);
		unlock(np);
		return;
	}

	uncachepage(np);
	np->va = p->va;
	np->daddr = p->daddr;
	copypage(p, np);
	cachepage(np, p->image);
	unlock(np);
	uncachepage(p);
}

void
copypage(Page *f, Page *t)
{
	KMap *ks, *kd;

	ks = kmap(f);
	kd = kmap(t);
	memmove((void*)VA(kd), (void*)VA(ks), BY2PG);
	kunmap(ks);
	kunmap(kd);
}

void
uncachepage(Page *p)			/* Always called with a locked page */
{
	Page **l, *f;

	if(p->image == 0)
		return;

	lock(&palloc.hashlock);
	l = &pghash(p->daddr);
	for(f = *l; f; f = f->hash) {
		if(f == p) {
			*l = p->hash;
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
	putimage(p->image);
	p->image = 0;
	p->daddr = 0;
}

void
cachepage(Page *p, Image *i)
{
	Page **l;

	/* If this ever happens it should be fixed by calling
	 * uncachepage instead of panic. I think there is a race
	 * with pio in which this can happen. Calling uncachepage is
	 * correct - I just wanted to see if we got here.
	 */
	if(p->image)
		panic("cachepage");

	incref(i);
	lock(&palloc.hashlock);
	p->image = i;
	l = &pghash(p->daddr);
	p->hash = *l;
	*l = p;
	unlock(&palloc.hashlock);
}

void
cachedel(Image *i, ulong daddr)
{
	Page *f, **l;

	lock(&palloc.hashlock);
	l = &pghash(daddr);
	for(f = *l; f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			*l = f->hash;
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
}

Page *
lookpage(Image *i, ulong daddr)
{
	Page *f;

	lock(&palloc.hashlock);
	for(f = pghash(daddr); f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			unlock(&palloc.hashlock);

			lock(f);
			if(f->image != i || f->daddr != daddr) {
				unlock(f);
				return 0;
			}

			lock(&palloc);
			if(++f->ref == 1) {
				if(f->prev)
					f->prev->next = f->next;
				else
					palloc.head = f->next;

				if(f->next)
					f->next->prev = f->prev;
				else
					palloc.tail = f->prev;

				palloc.freecount--;
			}
			unlock(&palloc);

			unlock(f);
			return f;
		}
	}
	unlock(&palloc.hashlock);
	return 0;
}

Pte*
ptecpy(Pte *old)
{
	Pte *new;
	Page **src, **dst;

	new = ptealloc();
	dst = &new->pages[old->first-old->pages];
	new->first = dst;
	for(src = old->first; src <= old->last; src++, dst++)
		if(*src) {
			if(onswap(*src))
				dupswap(*src);
			else {
				lock(*src);
				(*src)->ref++;
				unlock(*src);
			}
			new->last = dst;
			*dst = *src;
		}

	return new;
}

Pte*
ptealloc(void)
{
	Pte *new;

	new = smalloc(sizeof(Pte));
	new->first = &new->pages[PTEPERTAB];
	new->last = new->pages;
	return new;
}

void
freepte(Segment *s, Pte *p)
{
	int ref;
	void (*fn)(Page*);
	Page *pt, **pg, **ptop;

	switch(s->type&SG_TYPE) {
	case SG_PHYSICAL:
		fn = s->pseg->pgfree;
		ptop = &p->pages[PTEPERTAB];
		if(fn) {
			for(pg = p->pages; pg < ptop; pg++) {
				if(*pg == 0)
					continue;
				(*fn)(*pg);
				*pg = 0;
			}
			break;
		}
		for(pg = p->pages; pg < ptop; pg++) {
			pt = *pg;
			if(pt == 0)
				continue;
			lock(pt);
			ref = --pt->ref;
			unlock(pt);
			if(ref == 0)
				free(pt);
		}
		break;
	default:
		for(pg = p->first; pg <= p->last; pg++)
			if(*pg) {
				putpage(*pg);
				*pg = 0;
			}
	}
	free(p);
}
