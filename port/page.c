#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

#define PGHFUN(x, y)	(((ulong)x^(ulong)y)%PGHSIZE)
#define	pghash(s)	palloc.hash[PGHFUN(s->image, p->daddr)]

struct Palloc palloc;

struct Ptealloc
{
	Lock;
	Pte	*free;
	int	pages;
}ptealloclk;

extern long end;
static Lock pglock;

/* Multiplex a hardware lock for per page manipulations */
void
lockpage(Page *p)
{	
	int s;

	for(;;) {
		if(p->lock == 0) {
			s = splhi();
			lock(&pglock);
			if(p->lock == 0) {
				p->lock = 1;
				unlock(&pglock);
				splx(s);
				return;
			}
			unlock(&pglock);
			splx(s);
		}
		sched();
	}
}

void
unlockpage(Page *p)
{
	p->lock = 0;
}

/*
 *  Called to allocate permanent data structures, before calling pageinit().
 *  We assume all of text+data+bss is in the first memory bank.
 */
void*
ialloc(ulong n, int align)
{
	ulong p;
	ulong *ap;

	if(palloc.active && n!=0)
		print("ialloc bad\n");

	if(palloc.addr0 == 0){
		/* addr0 and addr1 are physical addresses */
		palloc.addr0 = (((ulong)&end)&~KZERO) + conf.base0;
		palloc.addr1 = conf.base1;
	}

	/*
	 *  try first bank
	 */
	p = align ? PGROUND(palloc.addr0) : palloc.addr0;
	if(p+n > conf.base0 + (conf.npage0<<PGSHIFT)){
		/*
		 *  no room in first bank, try second bank
		 */
		if(conf.npage1 <= 0)
			panic("keep bill joy away 1");
		p = align ? PGROUND(palloc.addr1) : palloc.addr1;
		ap = &palloc.addr1;
	} else
		ap = &palloc.addr0;

	if(p >= conf.maxialloc)
		panic("keep bill joy away 2");

	/*
	 *  zero it
	 */
	memset((void*)(p|KZERO), 0, n);

	/*
	 *  don't put anything else into a page aligned ialloc
	 */
	*ap = align ? PGROUND(p+n) : (p+n);

	return (void*)(p|KZERO);
}

void
pageinit(void)
{
	ulong np, addr, lim;
	ulong i, vmem, pmem;
	Page *p;

	/*
	 *  calculate an upper bound to the number of pages structures
	 *  we'll need (np).
	 */
	np = (conf.npage0<<PGSHIFT) - (palloc.addr0 - conf.base0);
	np += (conf.npage1<<PGSHIFT) - (palloc.addr1 - conf.base1);
	np = np>>PGSHIFT;

	/*
	 *  allocate Page structs (no more ialloc's allowed after this).
	 *  np is useless after this ialloc since we've just eaten up
	 *  some pages for the Page structures.
	 */
	palloc.head = ialloc(np*sizeof(Page), 0);
	palloc.active = 1;

	/*
 	 *  for each page in each bank, point a page structure to
	 *  the page and chain it into the free list
	 */
	p = palloc.head;
	addr = palloc.addr0 = PGROUND(palloc.addr0);
	lim = conf.base0 + (conf.npage0<<PGSHIFT);
	for(; addr < lim; addr += BY2PG){
		p->next = p+1;
		p->prev = p-1;
		p->pa = addr;
		p++;
	}
	addr = palloc.addr1 = PGROUND(palloc.addr1);
	lim = conf.base1 + (conf.npage1<<PGSHIFT);
	for(; addr < lim; addr += BY2PG){
		p->next = p+1;
		p->prev = p-1;
		p->pa = addr;
		p++;
	}
	palloc.tail = p - 1;
	palloc.head->prev = 0;
	palloc.tail->next = 0;

	palloc.user = palloc.freecount = p - palloc.head;
	pmem = palloc.user*BY2PG/1024;
	vmem = pmem + ((conf.nswap)*BY2PG)/1024;
	print("%lud free pages, %dK bytes, swap %dK bytes\n", palloc.user, pmem, vmem);
}

Page*
newpage(int clear, Segment **s, ulong va)
{
	Page *p;
	KMap *k;
	int i;

	if(palloc.active == 0)
		print("newpage inactive\n");

	lock(&palloc);

	/* The kp test is a poor guard against the pager deadlocking */
	while((palloc.freecount < HIGHWATER && u->p->kp == 0) || palloc.freecount == 0) {
		palloc.wanted++;
		unlock(&palloc);
		if(s && *s) {
			qunlock(&((*s)->lk));
			*s = 0;
		}
		qlock(&palloc.pwait);			/* Hold memory requesters here */

		if(waserror()) {
			qunlock(&palloc.pwait);
			lock(&palloc);
			palloc.wanted--;
			unlock(&palloc);
			nexterror();
		}

		kickpager();
		tsleep(&palloc.r, ispages, 0, 1000);

		poperror();

		qunlock(&palloc.pwait);
		lock(&palloc);
		palloc.wanted--;
	}

	p = palloc.head;
	if(palloc.head = p->next)		/* = Assign */
		palloc.head->prev = 0;
	else
		palloc.tail = 0;

	palloc.freecount--;
	unlock(&palloc);

	lockpage(p);

	if(p->ref != 0)
		panic("newpage");

	uncachepage(p);
	p->ref++;
	p->va = va;
	p->modref = 0;
	for(i = 0; i < MAXMACH; i++)
		p->cachectl[i] = PG_NOFLUSH;
	unlockpage(p);

	if(clear){
		k = kmap(p);
		memset((void*)VA(k), 0, BY2PG);
		kunmap(k);
	}

	return p;
}

int
ispages(void *p)
{
	return palloc.freecount >= HIGHWATER;
}

void
putpage(Page *p)
{
	int count;

	if(onswap(p)) {
		putswap(p);
		return;
	}

	lockpage(p);
	if(--p->ref == 0) {
		lock(&palloc);
		if(p->image) {
			if(palloc.tail) {
				p->prev = palloc.tail;
				palloc.tail->next = p;
				p->next = 0;
				palloc.tail = p;
			}
			else {
				palloc.head = palloc.tail = p;
				p->prev = p->next = 0;
			}
		}
		else {
			if(palloc.head) {
				p->next = palloc.head;
				palloc.head->prev = p;
				p->prev = 0;
				palloc.head = p;
			}
			else {
				palloc.head = palloc.tail = p;
				p->prev = p->next = 0;
			}
		}

		palloc.freecount++;		/* Release people waiting for memory */
		unlock(&palloc);
	}
	unlockpage(p);

	if(palloc.wanted)
		wakeup(&palloc.r);
}

void
duppage(Page *p)				/* Always call with p locked */
{
	Page *np;

	lock(&palloc);

	if(palloc.freecount < HIGHWATER || /* No freelist cache when memory is very low */
	   p->image == &swapimage) {	   /* No dup for swap pages */
		unlock(&palloc);
		uncachepage(p);	
		return;
	}

	np = palloc.head;			/* Allocate a new page from freelist */
	if(palloc.head = np->next)		/* = Assign */
		palloc.head->prev = 0;
	else
		palloc.tail = 0;

	if(palloc.tail) {			/* Link back onto tail to give us lru */
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

	lockpage(np);				/* Cache the new version */
	if(np->ref != 0) {			/* Stolen by new page */
		uncachepage(p);
		unlockpage(np);
		return;
	}
	
	uncachepage(np);
	np->va = p->va;
	np->daddr = p->daddr;
	copypage(p, np);
	cachepage(np, p->image);
	unlockpage(np);
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
uncachepage(Page *p)				/* Always called with a locked page */
{
	Page **l, *f;

	if(p->image) {
		lock(&palloc.hashlock);
		l = &pghash(p);
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
	}
}

void
cachepage(Page *p, Image *i)
{
	Page **l;

	incref(i);
	lock(&palloc.hashlock);
	p->image = i;
	l = &pghash(p);
	p->hash = *l;
	*l = p;
	unlock(&palloc.hashlock);
}

Page *
lookpage(Image *i, ulong daddr)
{
	Page *f;

	lock(&palloc.hashlock);
	for(f = palloc.hash[PGHFUN(i, daddr)]; f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			unlock(&palloc.hashlock);

			lockpage(f);
			if(f->image != i || f->daddr != daddr) {
				unlockpage(f);
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

			unlockpage(f);
			return f;	
		}
	}
	unlock(&palloc.hashlock);
	return 0;
}

Pte*
ptecpy(Pte *old)
{
	Page **src, **dst, **end;
	Pte *new;

	new = ptealloc();

	end = &old->pages[PTEPERTAB];
	for(src = old->pages, dst = new->pages; src < end; src++, dst++)
		if(*src) {
			if(onswap(*src))
				dupswap(*src);
			else {
				lockpage(*src);
				(*src)->ref++;
				unlockpage(*src);
			}
			*dst = *src;
		}

	return new;		
}

Pte*
ptealloc(void)
{
	Pte *new;
	int i, n;
	KMap *k;

	lock(&ptealloclk);
	while(ptealloclk.free == 0) {
		unlock(&ptealloclk);

		k = kmap(newpage(1, 0, 0));
		new = (Pte*)VA(k);
		n = (BY2PG/sizeof(Pte))-1;
		for(i = 0; i < n; i++)
			new[i].next = &new[i+1];

		lock(&ptealloclk);
		ptealloclk.pages++;
		new[i].next = ptealloclk.free;
		ptealloclk.free = new;
	}

	new = ptealloclk.free;
	ptealloclk.free = new->next;
	unlock(&ptealloclk);
	memset(new->pages, 0, sizeof(new->pages));
	return new;
}

void
freepte(Segment *s, Pte *p)
{
	Page **pg, **ptop;

	ptop = &p->pages[PTEPERTAB];

	switch(s->type&SG_TYPE) {
	case SG_PHYSICAL:
		for(pg = p->pages; pg < ptop; pg++)
			if(*pg)
				(*s->pgfree)(*pg);
		break;
	default:
		for(pg = p->pages; pg < ptop; pg++)
			if(*pg)
				putpage(*pg);
	}

	lock(&ptealloclk);
	p->next = ptealloclk.free;
	ptealloclk.free = p;
	unlock(&ptealloclk);
}
