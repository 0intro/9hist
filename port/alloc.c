#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#define	nil	((void*)0)
#define datoff	((int)&((Xhdr*)0)->data)

enum
{
	Nhole	= 128,
	Magic	= 0xDeadBabe,
};

typedef struct Hole Hole;
typedef struct Xalloc Xalloc;
typedef struct Xhdr Xhdr;
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

Xalloc	xlists;

void
xinit(void)
{
	Hole *h, *eh;

	eh = &xlists.hole[Nhole-1];
	for(h = xlists.hole; h < eh; h++)
		h->link = h+1;

	xlists.flist = h;
}

void
xhole(ulong addr, ulong size)
{
	Hole *h, **l;

	lock(&xlists);
	l = &xlists.table;
	for(h = *l; h; h = h->link) {
		if(h->top == addr) {
			h->size += size;
			h->top = h->addr+h->size;
			unlock(&xlists);
			return;
		}
		if(h->addr > addr)
			break;
		l = &h->link;
	}

	if(xlists.flist == nil) {
		print("xfree: no free holes, leaked %d bytes\n", size);
		unlock(&xlists);
		return;
	}

	h = xlists.flist;
	xlists.flist = h->link;
	h->addr = addr;
	h->top = addr+size;
	h->size = size;
	h->link = *l;
	*l = h;
	unlock(&xlists);
}

void*
xalloc(ulong size)
{
	Hole *h, **l;
	Xhdr *p;

	size += sizeof(Xhdr);

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
			p->magix = Magic;
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
	if(x->magix != Magic)
		panic("xfree");

	xhole((ulong)x, x->size);
}

void
xsummary(void)
{
	int i;
	Hole *h;

	i = 0;
	for(h = xlists.flist; h; h = h->link)
		i++;

	print("%d holes free\n", i);
	i = 0;
	for(h = xlists.table; h; h = h->link) {
		print("%lux %lux %d\n", h->addr, h->top, h->size);
		i += h->size;
	}
	print("%d bytes free\n", i);
}
