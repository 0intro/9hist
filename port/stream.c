#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

typedef struct Chunk	Chunk;
typedef	struct Chunkl	Chunkl;
typedef	struct Arena	Arena;

enum
{
	Minpow= 7,
	Maxpow=	12,
};

struct Chunk
{
	Chunk	*next;
};

struct Alloc
{
	Lock;
	Chunk	*first;
	int	had;
	int	goal;
	int	last;
};

struct Arena
{
	Chunkl	alloc[Maxpow-Minpow+1];
	Chunkl	freed;
};

static Arena arena;

/*
 *  Manage interrupt level memory allocation.
 */
static void
iallockproc(void *arg)
{
	Chunk *p, *first, **l;
	Chunkl *cl;
	int pow, x, i;

	USED(arg);
	for(;;){
		tsleep(&freed->r, return0, 0, 500);

		/* really free what was freed at interrupt level */
		cl = &arena.freed;
		if(cl->first){
			x = slphi();
			lock(cl);
			first = cl->first;
			cl->first = 0;
			unlock(cl);
			spllo();
	
			for(; first; first = p){
				p = first->next;
				free(first);
			}
		}

		/* make sure we have blocks available for interrupt level */
		for(pow = Minpow; pow <= Maxpow; pow++){
			cl = &arena.alloc[pow];
			if(cl->have >= cl->goal){
				cl->had = cl->have;
				continue;
			}

			/* increase goal if we've been drained twice in a row */
			if(cl->have == 0 && cl->had == 0)
				cl->goal += cl->goal>>2;
			cl->had = cl->have;
			l = &first;
			for(i = x = cl->goal - cl->have; x > 0; x--){
				p = alloc(1<<pow);
				if(p == 0)
					break;
				*l = p;
				l = &p->next;
			}
			if(first){
				x = splhi();
				lock(cl);
				*l = cl->first;
				cl->first = first;
				cl->have += i;
				unlock(cl);
				spllo(x);
			}
		}
	}
}

void
iallocinit(void)
{
	int pow;
	Chunkl *cl;

	for(pow = Minpow; pow <= Maxpow; pow++){
		cl = &arena.alloc[pow];
		cl->goal = Maxpow-pow + 4;
	}

	/* start garbage collector */
	kproc("iallockproc", iallockproc, 0);
}

void*
ialloc(int size)
{
	int pow;
	Chunkl *cl;
	Chunk *p;

	for(pow = Min; pow <= Maxpow; pow++)
		if(size <= (1<<pow)){
			cl = &arena.alloc[pow];
			lock(cl);
			p = cl->first;
			if(p){
				cl->have--;
				cl->first = p->next;
			}
			unlock(cl);
			return (void*)p;
		}
	panic("ialloc %d\n", size);
}

void
ifree(void *a)
{
	Chunk *p;
	Chunkl *cl;

	cl = &arena.freed;
	p = a;
	lock(cl);
	p->next = cl->first;
	cl->first = p;
	unlock(cl);
}

/*
 *  allocate queues and blocks
 */
Block*
allocb(int size)
{
	Block *b;

	b = alloc(sizeof(Block) + size);
	if(b == 0)
		exhausted("blocks");

	b->base = (uchar*)(b+1);
	b->rp = b->wp = b->base;
	b->lim = b->base + size;

	return b;
}

/*
 *  Interrupt level copy out of a queue, return # bytes copied.  If drop is
 *  set, any bytes left in a block afer a consume are discarded.
 */
int
consume(Queue *q, uchar *p, int len, int drop)
{
	Block *b;
	int n;

	lock(q);
	b = q->first;
	if(b == 0){
		q->state |= Qstarve;
		unlock(q);
		return -1;
	}
	n = BLEN(b);
	if(n < len)
		len = n;
	memmove(p, b->rp, len);
	if(len == n || drop){
		q->first = b->next;
		ifree(b);
	} else
		b->rp += len;
	unlock(q);
	return len;
}

int
produce(Queue *q, uchar *p, int len)
{
	Block *b;

	b = ialloc(sizeof(Block)
}
