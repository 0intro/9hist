#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 *  interrupt level memory allocation
 */
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

struct Chunkl
{
	Lock;
	Chunk	*first;
	int	have;
	int	goal;
	int	hist;
};

struct Arena
{
	Chunkl	alloc[Maxpow+1];
	Chunkl	freed;
	Rendez r;
};

static Arena arena;

/*
 *  IO queues
 */
typedef struct Block	Block;
typedef struct Queue	Queue;

struct Block
{
	Block	*next;

	uchar	*rp;			/* first unconsumed byte */
	uchar	*wp;			/* first empty byte */
	uchar	*lim;			/* 1 past the end of the buffer */
	uchar	*base;			/* start of the buffer */
	uchar	flag;
};
#define BLEN(b)		((b)->wp - (b)->rp)

struct Queue
{
	Lock;

	Block	*bfirst;	/* buffer */
	Block	*blast;

	int	len;		/* bytes in queue */
	int	limit;		/* max bytes in queue */
	int	state;

	void	(*kick)(void*);	/* restart output */
	void	*arg;		/* argument to kick */

	QLock	rlock;		/* mutex for reading processes */
	Rendez	rr;		/* process waiting to read */
	QLock	wlock;		/* mutex for writing processes */
	Rendez	wr;		/* process waiting to write */
};

enum
{
	/* Block.flag */
	Bfilled=1,		/* block filled */

	/* Queue.state */	
	Qstarve=	(1<<0),		/* consumer starved */
	Qmsg=		(1<<1),		/* message stream */
	Qclosed=	(1<<2),
	Qflow=		(1<<3),
};

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
		tsleep(&arena.r, return0, 0, 500);

		/* really free what was freed at interrupt level */
		cl = &arena.freed;
		if(cl->first){
			x = splhi();
			lock(cl);
			first = cl->first;
			cl->first = 0;
			unlock(cl);
			splx(x);
	
			for(; first; first = p){
				p = first->next;
				free(first);
			}
		}

		/* make sure we have blocks available for interrupt level */
		for(pow = Minpow; pow <= Maxpow; pow++){
			cl = &arena.alloc[pow];

			/*
			 *  if we've been ahead of the game for a while
			 *  start giving blocks back to the general pool
			 */
			if(cl->have >= cl->goal){
				cl->hist = ((cl->hist<<1) | 1) & 0xff;
				if(cl->hist == 0xff && cl->goal > 8)
					cl->goal--;
				continue;
			} else
				cl->hist <<= 1;

			/*
			 *  increase goal if we've been drained, decrease
			 *  goal if we've had lots of blocks twice in a row.
			 */
			if(cl->have == 0)
				cl->goal += cl->goal>>2;

			first = 0;
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
				splx(x);
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
	kproc("ialloc", iallockproc, 0);
}

Block*
iallocb(int size)
{
	int pow;
	Chunkl *cl;
	Chunk *p;
	Block *b;

	size += sizeof(Block);
	for(pow = Minpow; pow <= Maxpow; pow++)
		if(size <= (1<<pow)){
			cl = &arena.alloc[pow];
			lock(cl);
			p = cl->first;
			if(p == 0){
				unlock(cl);
				return 0;
			}
			cl->have--;
			cl->first = p->next;
			unlock(cl);
			b = (Block *)p;
			memset(b, 0, sizeof(Block));
			b->base = (uchar*)(b+1);
			b->wp = b->rp = b->base;
			b->lim = b->base + (1<<pow) - sizeof(Block);
			return b;
		}
	panic("iallocb %d\n", size);
	return 0;			/* not reached */
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

	memset(b, 0, sizeof(Block));
	b->base = (uchar*)(b+1);
	b->rp = b->wp = b->base;
	b->lim = b->base + size;
	b->flag = 0;

	return b;
}

/*
 *  Interrupt level copy out of a queue, return # bytes copied.  If drop is
 *  set, any bytes left in a block afer a consume are discarded.
 */
int
qconsume(Queue *q, void *vp, int len)
{
	Block *b;
	int n, dowakeup;
	uchar *p = vp;

	/* sync with qwrite */
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
	if((q->state & Qmsg) || len == n)
		q->bfirst = b->next;
	else
		b->rp += len;
	q->len -= len;

	/* if writer flow controlled, restart */
	if((q->state & Qflow) && q->len < q->limit/2){
		q->state &= ~Qflow;
		dowakeup = 1;
	} else
		dowakeup = 0;

	unlock(q);

	if(dowakeup)
		wakeup(&q->wr);

	/* discard the block if we're done with it */
	if((q->state & Qmsg) || len == n)
		ifree(b);

	return len;
}

int
qproduce(Queue *q, void *vp, int len)
{
	Block *b;
	int dowakeup;
	uchar *p = vp;

	/* sync with qread */
	lock(q);

	/* no waiting receivers, room in buffer? */
	if(q->len >= q->limit){
		unlock(q);
		return -1;
	}

	/* save in buffer */
	b = q->bfirst;
	if((q->state & Qmsg) == 0 && b && b->lim - b->wp <= len){
		memmove(b->wp, p, len);
		b->wp += len;
		b->flag |= Bfilled;
	} else {
		b = iallocb(len);
		if(b == 0){
			unlock(q);
			return -1;
		}
		memmove(b->wp, p, len);
		b->wp += len;
		if(q->bfirst)
			q->blast->next = b;
		else
			q->bfirst = b;
		q->blast = b;
	}
	q->len += len;
	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	} else
		dowakeup = 0;
	unlock(q);

	if(dowakeup){
		if(q->kick)
			(*q->kick)(q->arg);
		wakeup(&q->rr);
	}

	return len;
}

/*
 *  called by non-interrupt code
 */
Queue*
qopen(int limit, int msg, void (*kick)(void*), void *arg)
{
	Queue *q;

	q = malloc(sizeof(Queue));
	if(q == 0)
		return 0;

	memset(q, 0, sizeof(Queue));
	q->limit = limit;
	q->kick = kick;
	q->arg = arg;
	q->state = msg ? Qmsg : 0;
	q->state |= Qstarve;

	return q;
}

static int
notempty(void *a)
{
	Queue *q = a;

	return q->bfirst != 0;
}

/*
 *  read a queue.  if no data is queued, post a Block
 *  and wait on its Rendez.
 */
long
qread(Queue *q, void *vp, int len)
{
	Block *b;
	int x, n, dowakeup;
	uchar *p = vp;

	qlock(&q->rlock);
	if(waserror()){
		qunlock(&q->rlock);
		nexterror();
	}

	/* wait for data */
	for(;;){
		/* sync with qwrite/qproduce */
		x = splhi();
		lock(q);

		b = q->bfirst;
		if(b)
			break;

		if(q->state & Qclosed){
			unlock(q);
			splx(x);
			return 0;
		}

		q->state |= Qstarve;
		unlock(q);
		splx(x);
		sleep(&q->rr, notempty, q);
	}

	/* remove a buffered block */
	q->bfirst = b->next;
	n = BLEN(b);
	q->len -= n;

	/* if writer flow controlled, restart */
	if((q->state & Qflow) && q->len < q->limit/2){
		q->state &= ~Qflow;
		dowakeup = 1;
	} else
		dowakeup = 0;
	unlock(q);
	splx(x);

	/* do this outside of the lock(q)! */
	if(n > len)
		n = len;
	memmove(p, b->rp, n);
	b->rp += n;

	/* free it or put what's left on the queue */
	if(b->rp >= b->wp || (q->state&Qmsg))
		free(b);
	else {
		x = splhi();
		lock(q);
		b->next = q->bfirst;
		q->bfirst = b;
		q->len += BLEN(b);
		unlock(q);
		splx(x);
	}

	/* wakeup flow controlled writers (with a bit of histeresis) */
	if(dowakeup)
		wakeup(&q->wr);

	poperror();
	qunlock(&q->rlock);
	return n;
}

static int
qnotfull(void *a)
{
	Queue *q = a;

	return q->len < q->limit;
}

/*
 *  write to a queue.  if no reader blocks are posted
 *  queue the data.
 */
long
qwrite(Queue *q, void *vp, int len, int nowait)
{
	int x, dowakeup;
	Block *b;
	uchar *p = vp;

	b = allocb(len);
	memmove(b->wp, p, len);
	b->wp += len;

	/* flow control */
	while(!qnotfull(q)){
		if(nowait)
			return len;
		qlock(&q->wlock);
		q->state |= Qflow;
		sleep(&q->wr, qnotfull, q);
		qunlock(&q->wlock);
	}

	x = splhi();
	lock(q);

	if(q->state & Qclosed){
		unlock(q);
		splx(x);
		error(Ehungup);
	}

	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += len;

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	} else
		dowakeup = 0;

	unlock(q);
	splx(x);

	if(dowakeup){
		if(q->kick)
			(*q->kick)(q->arg);
		wakeup(&q->rr);
	}

	return len;
}

/*
 *  Mark a queue as closed.  No further IO is permitted.
 *  All blocks are released.
 */
void
qclose(Queue *q)
{
	int x;
	Block *b, *bfirst;

	/* mark it */
	x = splhi();
	lock(q);
	q->state |= Qclosed;
	bfirst = q->bfirst;
	q->bfirst = 0;
	unlock(q);
	splx(x);

	/* free queued blocks */
	while(b = bfirst){
		bfirst = b->next;
		free(b);
	}

	/* wake up readers/writers */
	wakeup(&q->rr);
	wakeup(&q->wr);
}

/*
 *  Mark a queue as closed.  Wakeup any readers.  Don't remove queued
 *  blocks.
 */
void
qhangup(Queue *q)
{
	int x;

	/* mark it */
	x = splhi();
	lock(q);
	q->state |= Qclosed;
	unlock(q);
	splx(x);

	/* wake up readers/writers */
	wakeup(&q->rr);
	wakeup(&q->wr);
}

/*
 *  mark a queue as no longer hung up
 */
void
qreopen(Queue *q)
{
	q->state &= ~Qclosed;
	q->state |= Qstarve;
}

/*
 *  return bytes queued
 */
int
qlen(Queue *q)
{
	return q->len;
}

/*
 *  return true if we can read without blocking
 */
int
qcanread(Queue *q)
{
	return q->bfirst!=0;
}
