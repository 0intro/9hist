#include <u.h>
#include <libc.h>

enum
{
	MAGIC		= 0xDEADBABE,
	MAX2SIZE	= 20
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
	Bucket	*btab[MAX2SIZE];	
};

static Arena arena;
#define datoff		((int)&((Bucket*)0)->data)

void*
malloc(uint size)
{
	int pow;
	Bucket *bp;

	for(pow = 1; pow < MAX2SIZE; pow++) {
		if(size <= (1<<pow))
			goto good;
	}

	return nil;
good:
	/* Allocate off this list */
	lock(&arena);
	bp = arena.btab[pow];
	if(bp) {
		arena.btab[pow] = bp->next;
		arena.unlock();

		if(bp->magic != 0)
			abort();

		bp->magic = MAGIC;

		memset(bp->data, 0,  size);
		return  bp->data;
	}
	unlock(&arena);
	size = sizeof(Bucket)+(1<<pow);
	bp = sbrk(size);
	if((int)bp < 0)
		return nil;

	bp->size = pow;
	bp->magic = MAGIC;

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
