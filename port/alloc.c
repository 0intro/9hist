#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * Plan 9 has two kernel allocators, the x... routines provide a first
 * fit hole allocator which should be used for permanent or large structures.
 * Routines are provided to allocate aligned memory which does not cross
 * arbitrary 2^n boundaries. A second allocator malloc, smalloc, free is
 * a 2n bucket allocator which steals from the x routines. This should
 * be used for small frequently used structures.
 */

#define	nil		((void*)0)
#define datoff		((ulong)((Xhdr*)0)->data)
#define bdatoff		((ulong)((Bucket*)0)->data)

enum
{
	Maxpow		= 16,
	Nhole		= 128,
	Magichole	= 0xDeadBabe,
	Magic2n		= 0xFeedBeef,
	Spanlist	= 64,
};

typedef struct Hole Hole;
typedef struct Xalloc Xalloc;
typedef struct Xhdr Xhdr;
typedef struct Bucket Bucket;
typedef struct Arena Arena;

struct Hole
{
	ulong	addr;
	ulong	size;
	ulong	top;
	Hole	*link;
};

struct Xhdr
{
	ulong	size;
	ulong	magix;
	char	data[1];
};

struct Xalloc
{
	Lock;
	Hole	hole[Nhole];
	Hole	*flist;
	Hole	*table;
};

struct Bucket
{
	int	size;
	int	magic;
	Bucket	*next;
	char	data[1];
};

struct Arena
{
	Lock;
	Bucket	*btab[Maxpow];
	int	nbuck[Maxpow];
};

static Arena	arena;
static Xalloc	xlists;

void
xinit(void)
{
	ulong ktop;
	Hole *h, *eh;
	int up, np0, np1;

	eh = &xlists.hole[Nhole-1];
	for(h = xlists.hole; h < eh; h++)
		h->link = h+1;

	xlists.flist = xlists.hole;

	ktop = PGROUND((ulong)end);
	ktop = PADDR(ktop);
	conf.npage0 -= ktop/BY2PG;
	conf.base0 += ktop;

	up = conf.upages;
	np1 = up;
	if(np1 > conf.npage1)
		np1 = conf.npage1;

	palloc.p1 = conf.base1;
	conf.base1 += np1*BY2PG;
	conf.npage1 -= np1;
	xhole(conf.base1, conf.npage1*BY2PG);
	up -= np1;

	np0 = up;
	if(np0 > conf.npage0)
		np0 = conf.npage0;

	palloc.p0 = conf.base0;
	conf.base0 += np0*BY2PG;
	conf.npage0 -= np0;
	xhole(conf.base0, conf.npage0*BY2PG);

	palloc.np0 = np0;
	palloc.np1 = np1;
}

/*
 * NB. spanalloc memory will cause a panic if free'd
 */
void*
xspanalloc(ulong size, int align, ulong span)
{
	int i, j;
	ulong a, p;
	ulong ptr[Spanlist];

	span = ~(span-1);
	for(i = 0; i < Spanlist; i++) {
		p = (ulong)xalloc(size+align);
		if(p == 0)
			panic("xspanalloc: size %d align %d span %d", size, align, span);

		a = p+align;
		a &= ~(align-1);
		if((a&span) == ((a+size)&span)) {
			for(j = 0; j < i; j++)
				xfree((void*)ptr[j]);

			return (void*)a;
		}
		ptr[i] = p;
	}
	panic("xspanalloc: spanlist");		
	return 0;
}

void*
xalloc(ulong size)
{
	Xhdr *p;
	Hole *h, **l;

	size += BY2WD + sizeof(Xhdr);
	size &= ~(BY2WD-1);

	lock(&xlists);
	l = &xlists.table;
	for(h = *l; h; h = h->link) {
		if(h->size >= size) {
			p = (Xhdr*)h->addr;
			h->addr += size;
			h->size -= size;
			if(h->size == 0) {
				*l = h->link;
				h->link = xlists.flist;
				xlists.flist = h;
			}
			p = KADDR(p);
			memset(p, 0, size);
			p->magix = Magichole;
			p->size = size;
			unlock(&xlists);
			return p->data;
		}
		l = &h->link;
	}
	unlock(&xlists);
	return nil;
}

void
xfree(void *p)
{
	Xhdr *x;

	x = (Xhdr*)((ulong)p - datoff);
	if(x->magix != Magichole)
		panic("xfree");

	xhole(PADDR(x), x->size);
}

void
xhole(ulong addr, ulong size)
{
	ulong top;
	Hole *h, *c, **l;

	if(size == 0)
		return;

	top = addr + size;
	lock(&xlists);
	l = &xlists.table;
	for(h = *l; h; h = h->link) {
		if(h->top == addr) {
			h->size += size;
			h->top = h->addr+h->size;
			c = h->link;
			if(c && h->top == c->addr) {
				h->top += c->size;
				h->size += c->size;
				h->link = c->link;
				c->link = xlists.flist;
				xlists.flist = c;
			}
			unlock(&xlists);
			return;
		}
		if(h->addr > addr)
			break;
		l = &h->link;
	}
	if(h && top == h->addr) {
		h->addr -= size;
		h->size += size;
		unlock(&xlists);
		return;
	}

	if(xlists.flist == nil) {
		unlock(&xlists);
		print("xfree: no free holes, leaked %d bytes\n", size);
		return;
	}

	h = xlists.flist;
	xlists.flist = h->link;
	h->addr = addr;
	h->top = top;
	h->size = size;
	h->link = *l;
	*l = h;
	unlock(&xlists);
}

void*
malloc(ulong size)
{
	int pow;
	Bucket *bp;

	for(pow = 3; pow < Maxpow; pow++)
		if(size <= (1<<pow))
			goto good;

	return nil;
good:
	/* Allocate off this list */
	lock(&arena);
	bp = arena.btab[pow];
	if(bp) {
		arena.btab[pow] = bp->next;
		unlock(&arena);

		if(bp->magic != 0)
			panic("malloc");

		bp->magic = Magic2n;

		memset(bp->data, 0,  size);
		return  bp->data;
	}
	arena.nbuck[pow]++;
	unlock(&arena);
	size = sizeof(Bucket)+(1<<pow);
	bp = xalloc(size);
	if(bp == nil)
		return nil;

	bp->size = pow;
	bp->magic = Magic2n;
	return bp->data;
}

void*
smalloc(ulong size)
{
	void *p;

	p = malloc(size);
	if(p == nil) {
		print("asking for %d\n", size);
		xsummary();
		panic("smalloc should sleep");
	}
	return p;
}

void
free(void *ptr)
{
	Bucket *bp, **l;

	bp = (Bucket*)((ulong)ptr - bdatoff);
	if(bp->magic != Magic2n)
		panic("free");

	bp->magic = 0;
	lock(&arena);
	l = &arena.btab[bp->size];
	bp->next = *l;
	*l = bp;
	unlock(&arena);
}

void
xsummary(void)
{
	Hole *h;
	Bucket *k;
	int i, nfree;

	i = 0;
	for(h = xlists.flist; h; h = h->link)
		i++;

	print("%d holes free\n", i);
	i = 0;
	for(h = xlists.table; h; h = h->link) {
		print("%.8lux %.8lux %d\n", h->addr, h->top, h->size);
		i += h->size;
	}
	print("%d bytes free\n", i);
	for(i = 3; i < Maxpow; i++) {
		if(arena.btab[i] == 0 && arena.nbuck[i] == 0)
			continue;
		nfree = 0;
		for(k = arena.btab[i]; k; k = k->next)
			nfree++;
		print("%8d %4d %4d\n", 1<<i, arena.nbuck[i], nfree);
	}
}
