#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"
#include	"pool.h"

enum {
	POOL_STUPID = 0x80000000,
};

static Pool pmainmem = {
	.name=	"Main",
	.maxsize=	4*1024*1024,
	.minarena=	128*1024,
	.quantum=	32,
	.alloc=	xalloc,
	.merge=	xmerge,
	.flags=	0,
};

static Pool pimagmem = {
	.name=	"Image",
	.maxsize=	16*1024*1024,
	.minarena=	2*1024*1024,
	.quantum=	32,
	.alloc=	xalloc,
	.merge=	xmerge,
	.flags=	0,
};

Pool*	mainmem = &pmainmem;
Pool*	imagmem = &pimagmem;

void*
smalloc(ulong size)
{
	void *v;
	ulong *l;

	for(;;) {
		v = poolalloc(mainmem, size+8);
		if(v != nil)
			break;
		tsleep(&up->sleep, return0, 0, 100);
	}
	l = v;
	l[0] = getcallerpc(&size);
	v = l+2;
	memset(v, 0, size);
	return v;
}

void*
malloc(ulong size)
{
	ulong *l;
	void *v;

	v = poolalloc(mainmem, size+8);
	if(v != nil) {
		l = v;
		l[0] = getcallerpc(&size);
		v = l+2;
		memset(v, 0, size);
	}
	return v;
}

void*
mallocz(ulong size, int clr)
{
	void *v;
	ulong *l;

	v = poolalloc(mainmem, size+8);
	if(v != nil) {
		l = v;
		l[0] = getcallerpc(&size);
		v = l+2;
	}
	if(clr && v != nil)
		memset(v, 0, size);
	return v;
}

void
free(void *v)
{
	if(v != nil) {
if(0)		if(mainmem->flags & POOL_STUPID) {
			print("A");
			delay(50);
		}
		poolfree(mainmem, (uchar*)v-8);
if(0)		if(mainmem->flags & POOL_STUPID) {
			print("a");
			delay(50);
		}
	}
}

void*
realloc(void *v, ulong size)
{
	ulong *l;
	void *nv;
	long nsize;

	if(size == 0) {
		free(v);
		return nil;
	}
	if(v == nil)
		return malloc(size);

	nv = (uchar*)v-8;
	nsize = size+8;
	if(l = poolrealloc(mainmem, nv, nsize))
		return l+2;
	return nil;
}

ulong
msize(void *v)
{
	return poolmsize(mainmem, (uchar*)v-8)-8;
}

void*
calloc(ulong n, ulong szelem)
{
	return mallocz(n*szelem, 1);
}

void
poolsummary(Pool *p)
{
	print("%s max %lud cur %lud free %lud alloc %lud\n", p->name,
		p->maxsize, p->cursize, p->curfree, p->curalloc);
}

void
mallocsummary(void)
{
	poolsummary(mainmem);
	poolsummary(imagmem);
}

void
tagwithpc(void *v, ulong pc)
{
	ulong *u;
	if(v == nil)
		return;
	u = v;
	u[-2] = pc;
}

