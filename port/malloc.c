#include <u.h>
#include <libc.h>

enum
{
	MAGIC		= 0xDEADBABE,
	MAXPOW		= 20
};

typedef struct Bucket Bucket;
struct Bucket
{
	int	size;
	int	magic;
	Bucket	*next;
	char	data[1];
};

typedef struct Arena Arena;
struct Arena
{
	Lock;
	Bucket	*btab[MAXPOW];
};
static Arena arena;
#define datoff		((int)&((Bucket*)0)->data)

void*
malloc(uint size)
{
	int pow;
	Bucket *bp;

	size += sizeof(Bucket);

	for(pow = 1; pow < MAXPOW; pow++)
		if(size <= (1<<pow))
			goto good;

	return 0;

good:
	lock(&arena);
	bp = arena.btab[pow];
	if(bp) {
		arena.btab[pow] = bp->next;
		arena.unlock();

		if(bp->magic != 0)
			panic("malloc");

		bp->magic = MAGIC;

		memset(bp->data, 0,  size);
		return  bp->data;
	}
	unlock(&arena);

	size = sizeof(Bucket)+(1<<pow);
	bp = xbrk(size);
	if((int)bp < 0)
		return nil;

	memset(bp->data, 0,  size);
	return bp->data;
}

void
free(void *ptr)
{
	Bucket *bp, **l;

	/* Find the start of the structure */
	bp = (Bucket*)((uint)ptr - datoff);

	if(bp->magic != MAGIC)
		panic("free");

	bp->magic = 0;
	lock(&arena);
	l = &arena.btab[bp->size];
	bp->next = *l;
	*l = bp;
	unlock(&arena);
}
