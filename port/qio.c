#include	"u.h"
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
	kproc("iallockproc", iallockproc, 0);
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
			if(p){
				cl->have--;
				cl->first = p->next;
			}
			unlock(cl);
			b = (Block *)p;
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
qconsume(Queue *q, uchar *p, int len)
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
	if((q->state&Qmsg) || len == n)
		q->bfirst = b->next;
	else
		b->rp += len;
	q->len -= len;

	/* wakeup flow controlled writers (with a bit of histeresis) */
	if(q->len+len >= q->limit && q->len < q->limit/2)
		wakeup(&q->r);

	unlock(q);

	if((q->state&Qmsg) || len == n)
		ifree(b);

	return len;
}

static int
qproduce0(Queue *q, uchar *p, int len)
{
	Block *b;
	int n;

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

	/* no waiting receivers, room in buffer? */
	if(q->len >= q->limit){
		unlock(q);
		return -1;
	}

	/* save in buffer */
	b = q->bfirst;
	if((q->state&Qmsg)==0 && b && b->lim-b->wp <= len){
		memmove(b->wp, p, len);
		b->wp += len;
	} else {
		b = iallocb(len);
		if(b == 0){
			unlock(q);
			return -1;
		}
		b->wp += len;
		memmove(b->rp, p, len);
		if(q->bfirst)
			q->blast->next = b;
		else
			q->bfirst = b;
		q->blast = b;
	}
	q->len += len;
	unlock(q);
	return len;
}

int
qproduce(Queue *q, uchar *p, int len)
{
	int n, sofar;

	if(q->state&Qmsg)
		return qproduce0(q, p, len);

	for(sofar = 0; sofar < len; sofar += n){
		n = qproduce0(q, p+sofar, len-sofar);
		if(n < 0)
			break;
	}
	return sofar;
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
	q->state = Qmsg;

	return q;
}

static int
bfilled(void *a)
{
	Block *b = a;

	return b->wp - b->rp;
}

long
qread(Queue *q, char *p, int len)
{
	Block *b, *bb;
	int x, n;

	qlock(&q->rlock);

	/* ... to be replaced by a kmapping if need be */
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
		qunlock(&q->rlock);
		sleep(&b->r, bfilled, b);
		n = BLEN(b);
		memmove(p, b->rp, n);
		free(b);
		return n;
	}

	/* copy from a buffered block */
	q->bfirst = bb->next;
	n = BLEN(bb);
	if(n > len)
		n = len;
	q->len -= n;
	unlock(q);
	splx(x);
	memmove(p, bb->rp, n);
	bb->rp += n;

	/* free it or put it back */
	if(drop || bb->rp == bb->wp)
		free(bb);
	else {
		x = splhi();
		lock(q);
		bb->next = q->bfirst;
		q->bfirst = bb;
		unlock(q);
		splx(x);
	}
	qunlock(&q->rlock);
	free(b);
	return n;
}

static int
qnotfull(void *a)
{
	Queue *q = a;

	return q->len < q->limit;
}

static long
qwrite0(Queue *q, char *p, int len)
{
	Block *b, *bb;
	int x, n;

	b = allocb(len);

	x = splhi();
	lock(q);
	bb = q->rfirst;
	if(bb){
		/* hand to waiting receiver */
		n = bb->lim - bb->wp;
		q->rfirst = bb->next;
		unlock(q);
		splx(x);

		if(n < len)
			len = n;
		memmove(bb->wp, p, len);
		bb->wp += len;
		wakeup(&bb->r);

		free(b);
		return len;
	}
		
	memmove(b->rp, p, len);
	b->wp += len;

	/* flow control */
	if(!qnotfull(q))
		sleep(&q->r, qnotfull, q);

	x = splhi();
	lock(q);
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += len;
	if((q->state & Qstarve) && q->kick){
		q->state &= ~Qstarve;
		(*q->kick)(q->arg);
	}
	unlock(q);
	splx(x);

	return len;
}

long
qwrite(Queue *q, char *p, int len)
{
	int n, sofar;

	qlock(&q->wlock);
	if(waserror()){
		qunlock(&q->wlock);
		nexterror();
	}

	if(q->state&Qmsg){
		sofar = qwrite0(q, p, len);
	} else {
		for(sofar = 0; sofar < len; sofar += n){
			n = qwrite0(q, p+sofar, len-sofar);
			if(n < 0)
				break;
		}
	}

	poperror();
	qunlock(&q->wlock);
	return sofar;
}
