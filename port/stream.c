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

			/*
			 *  increase goal if we've been drained, decrease
			 *  goal if we've had lots of blocks twice in a row.
			 */
			if(cl->have == 0)
				cl->goal += cl->goal>>2;
			else {
				x = cl->goal/2;
				if(cl->goal > 4 && cl->had > x && cl->have > x)
					cl->goal--;
			}

			cl->had = cl->have;
			l = &first;
			for(i = x = cl->goal - cl->have; x > 0; x--){
				p = malloc(1<<pow);
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

	b = malloc(sizeof(Block) + size);
	if(b == 0)
		exhausted("Blocks");

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
	b = q->bfirst;
	if(b == 0){
		q->state |= Qstarve;
		unlock(q);
		return -1;
	}
	n = BLEN(b);
	if(n < len)
		len = n;
	memmove(p, b->rp, len);
	if(drop || len == n)
		q->bfirst = b->next;
	else
		b->rp += len;
	q->len -= len;

	/* wakeup flow controlled writers */
	if(q->len+len >= q->limit && q->len < q->limit)
		wakeup(&q->r);

	unlock(q);

	if(drop || len == n)
		ifree(b);

	return len;
}

int
produce(Queue *q, uchar *p, int len, int append)
{
	Block *b;

	lock(q);
	b = q->rfirst;
	if(b){
		/* hand to waiting receiver */
		n = b->lim - b->wp;
		if(n < len)
			len = n;
		memmove(b->wp, p, len);
		b->wp += len;
		q->rfirst = b->next;
		wakeup(&b->r);
		unlock(q);
		return len;
	}

	/* no waiting receivers, buffer */
	if(q->len >= q->limit)
		return -1;
	b = q->first;
	if(append && b && b->lim-b->wp <= len){
		memmove(b->wp, p, len);
		b->wp += len;
	} else {
		b = ialloc(sizeof(Block)+len);
		if(b == 0)
			return -1;
		b->base = (uchar*)(b+1);
		b->rp = b->base;
		b->wp = b->lim = b->base + len;
		memmove(b->rp, p, len);
		if(q->bfirst)
			q->blast->next = b;
		else
			q->bfirst = b;
		q->last = b;
	}
	q->len += len;
	unlock(q);
	return len;
}

/*
 *  called by non-interrupt code
 */
Queue*
qopen(int limit, void (*kick)(void*), void *arg)
{
	Queue *q;

	q = malloc(sizeof(Queue));
	if(q == 0)
		exhausted("Queues");
	q->limit = limit;
	q->kick = kick;
	q->arg = arg;
}

static int
bfilled(void *a)
{
	Block *b = a;

	return b->wp - b->rp;
}

long
qread(Queue *q, char *p, int len, int drop)
{
	Block *b, *bb;
	int x, n;

	/* ... to be replaced by a mapping */
	b = allocb(len);

	x = splhi();
	lock(q);
	bb = q->bfirst;
	if(bb == 0){
		/* wait for our block to be filled */
		if(q->rfirst)
			q->rlast->next = b;
		else
			q->rfirst = b;
		q->rlast = b;
		unlock(q);
		splx(x);
		sleep(&b->r, bfilled, b);
		n = BLEN(b);
		memmove(p, b->rp, n);
		return n;
	}

	/* grab a block from the buffer */
	n = BLEN(b);
	if(drop || n <= len){
		q->bfirst = b->next;
		q->len -= n;
		unlock(q);
		slpx(x);
		memmove(p, b->rp, n);
	} else {
		n = len;
		q->len -= n;
		memmove(p, b->rp, n);
		b->rp += n;
		unlock(q);
		slpx(x);
	}
	free(b);
	return n;
}

static int
qnotfull(void *a)
{
	Queue *q = a;

	return q->len < q->limit;
}

long
qwrite(Queue *q, char *p, int len)
{
	Block *b;
	int x, n;

	b = allocb(len);
	memmove(b->rp, p, len);
	b->wp += len;

	/* flow control */
	if(!qnotfull(q)){
		qlock(&q->wlock);
		sleep(&q->r, qnotfull, q);
		qunlock(&q->wlock);
	}
		
	x = splhi();
	lock(q);
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += len;
	if((q->state & Qstarve) && q->kick){
		q->stat &= ~Qstarve;
		(*q->kick)(q->arg);
	}
	unlock(q);
	splx(x);

	return len;
}
