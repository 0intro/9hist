#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"


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
	Maxpow		= 18,
	CUTOFF		= 12,
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
	ulong	pc;
	char	data[1];
};

struct Arena
{
	Lock;
	Bucket	*btab[Maxpow];
	int	nbuck[Maxpow];
	QLock	rq;
	Rendez	r;
};

static Arena	arena;
static Xalloc	xlists;

void
xinit(void)
{
	Hole *h, *eh;
	int upages, np0, np1;

	eh = &xlists.hole[Nhole-1];
	for(h = xlists.hole; h < eh; h++)
		h->link = h+1;

	xlists.flist = xlists.hole;

	upages = conf.upages;
	np1 = upages;
	if(np1 > conf.npage1)
		np1 = conf.npage1;

	palloc.p1 = conf.base1 + (conf.npage1 - np1)*BY2PG;
	conf.npage1 -= np1;
	xhole(conf.base1, conf.npage1*BY2PG);
	conf.npage1 = conf.base1+(conf.npage1*BY2PG);
	upages -= np1;

	np0 = upages;
	if(np0 > conf.npage0)
		np0 = conf.npage0;

	palloc.p0 = conf.base0 + (conf.npage0 - np0)*BY2PG;
	conf.npage0 -= np0;
	xhole(conf.base0, conf.npage0*BY2PG);
	conf.npage0 = conf.base0+(conf.npage0*BY2PG);

	palloc.np0 = np0;
	palloc.np1 = np1;
	/* Save the bounds of kernel alloc memory for kernel mmu mapping */
	conf.base0 = (ulong)KADDR(conf.base0);
	conf.base1 = (ulong)KADDR(conf.base1);
	conf.npage0 = (ulong)KADDR(conf.npage0);
	conf.npage1 = (ulong)KADDR(conf.npage1);
}

/*
 * NB. spanalloc memory will cause a panic if free'd
 */
void*
xspanalloc(ulong size, int align, ulong span)
{
	ulong a, v, t;

	a = (ulong)xalloc(size+align+span);
	if(a == 0)
		panic("xspanalloc: %d %d %lux\n", size, align, span);

	if(span > 2) {
		v = (a + span) & ~(span-1);
		t = v - a;
		if(t > 0)
			xhole(PADDR(a), t);
		t = a + span - v;
		if(t > 0)
			xhole(PADDR(v+size+align), t);
	}
	else
		v = a;

	if(align > 1)
		v = (v + align) & ~(align-1);

	return (void*)v;
}

void*
xallocz(ulong size, int zero)
{
	Xhdr *p;
	Hole *h, **l;

	size += BY2V + sizeof(Xhdr);
	size &= ~(BY2V-1);

	ilock(&xlists);
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
			iunlock(&xlists);
			p = KADDR(p);
			if(zero)
				memset(p, 0, size);
			p->magix = Magichole;
			p->size = size;
			return p->data;
		}
		l = &h->link;
	}
	iunlock(&xlists);
	return nil;
}

void
xfree(void *p)
{
	Xhdr *x;

	x = (Xhdr*)((ulong)p - datoff);
	if(x->magix != Magichole) {
		xsummary();
		panic("xfree(0x%lux) 0x%lux!=0x%lux", p, Magichole, x->magix);
	}
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
	ilock(&xlists);
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
			iunlock(&xlists);
			return;
		}
		if(h->addr > addr)
			break;
		l = &h->link;
	}
	if(h && top == h->addr) {
		h->addr -= size;
		h->size += size;
		iunlock(&xlists);
		return;
	}

	if(xlists.flist == nil) {
		iunlock(&xlists);
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
	iunlock(&xlists);
}

void*
mallocz(ulong size, int zero)
{
	ulong next;
	int pow, n;
	Bucket *bp, *nbp;

	for(pow = 3; pow <= Maxpow; pow++)
		if(size <= (1<<pow))
			goto good;

	return nil;
good:
	/* Allocate off this list */
	ilock(&arena);
	bp = arena.btab[pow];
	if(bp) {
		arena.btab[pow] = bp->next;
		iunlock(&arena);

		if(bp->magic != 0)
			panic("malloc %lux %lux", bp->magic, bp->pc);

		bp->magic = Magic2n;

		if(zero)
			memset(bp->data, 0,  size);
		return  bp->data;
	}
	iunlock(&arena);

	size = sizeof(Bucket)+(1<<pow);
	size += BY2V;
	size &= ~(BY2V-1);

	if(pow < CUTOFF) {
		n = (CUTOFF-pow)+2;
		bp = xalloc(size*n);
		if(bp == nil)
			return nil;

		next = (ulong)bp+size;
		nbp = (Bucket*)next;
		ilock(&arena);
		arena.btab[pow] = nbp;
		arena.nbuck[pow] += n;
		for(n -= 2; n; n--) {
			next = (ulong)nbp+size;
			nbp->next = (Bucket*)next;
			nbp->size = pow;
			nbp = nbp->next;
		}
		nbp->size = pow;
		iunlock(&arena);
	}
	else {
		bp = xalloc(size);
		if(bp == nil)
			return nil;

		arena.nbuck[pow]++;
	}

	bp->size = pow;
	bp->magic = Magic2n;

	return bp->data;
}

void*
malloc(ulong size)
{
	return mallocz(size, 1);
}

void*
smalloc(ulong size)
{
	char *s;
	void *p;
	int attempt;

	for(attempt = 0; attempt < 1000; attempt++) {
		p = malloc(size);
		if(p != nil)
			return p;
		s = up->psstate;
		up->psstate = "Malloc";
		qlock(&arena.rq);
		while(waserror())
			;
		sleep(&arena.r, return0, nil);
		poperror();
		qunlock(&arena.rq);
		up->psstate = s;
	}
	pexit(Enomem, 1);
	return 0;
}

int
msize(void *ptr)
{
	Bucket *bp;

	bp = (Bucket*)((ulong)ptr - bdatoff);
	if(bp->magic != Magic2n)
		panic("msize");
	return 1<<bp->size;
}

void
free(void *ptr)
{
	Bucket *bp, **l;

	bp = (Bucket*)((ulong)ptr - bdatoff);
	if(bp->magic != Magic2n)
		panic("free %lux %lux", bp->magic, bp->pc);

	bp->magic = 0;
	ilock(&arena);
	l = &arena.btab[bp->size];
	bp->next = *l;
	*l = bp;
	iunlock(&arena);
	if(arena.r.p)
		wakeup(&arena.r);
}

void
xsummary(void)
{
	Hole *h;
	Bucket *k;
	int i, nfree, nused;

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
	nused = 0;
	for(i = 3; i < Maxpow; i++) {
		if(arena.btab[i] == 0 && arena.nbuck[i] == 0)
			continue;
		nused += arena.nbuck[i]*(1<<i);
		nfree = 0;
		for(k = arena.btab[i]; k; k = k->next)
			nfree++;
		print("%8d %4d %4d\n", 1<<i, arena.nbuck[i], nfree);
	}
	print("%d bytes in pool\n", nused);
}
