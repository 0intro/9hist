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
			if(p == 0){
				unlock(cl);
				return 0;
			}
			cl->have--;
			cl->first = p->next;
			unlock(cl);
			b = (Block *)p;
			b->base = (uchar*)(b+1);
			b->wp = b->rp = b->base;
			b->lim = b->base + (1<<pow) - sizeof(Block);
			b->flag = 0;
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
	b->flag = 0;

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

	/* wakeup flow controlled writers (with a bit of histeresis) */
	if(q->len+len >= q->limit && q->len < q->limit/2)
		wakeup(&q->r);

	unlock(q);

	if((q->state & Qmsg) || len == n)
		ifree(b);

	return len;
}

static int
qproduce0(Queue *q, uchar *p, int len)
{
	Block *b;
	int n;

	/* sync with qread */
	lock(q);

	b = q->rfirst;
	if(b){
		/* hand to waiting receiver */
		q->rfirst = b->next;
		unlock(q);
		n = b->lim - b->wp;
		if(n < len)
			len = n;
		memmove(b->wp, p, len);
		b->wp += len;
		b->flag |= Bfilled;
		wakeup(&b->r);
		return len;
	}

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
		b->flag |= Bfilled;
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

	sofar = 0;
	do {
		n = qproduce0(q, p + sofar, len - sofar);
		if(n < 0)
			break;
		sofar += n;
	} while(sofar < len && (q->state & Qmsg) == 0);
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

ulong qrtoomany;
ulong qrtoofew;

static int
bfilled(void *a)
{
	Block *b = a;

	return b->flag & Bfilled;
}

long
qread(Queue *q, char *p, int len)
{
	Block *b, *bb, **l;
	int x, n;

	qlock(&q->rlock);
	b = 0;
	if(waserror()){
		qunlock(&q->rlock);
		if(b)
			free(b);
		nexterror();
	}

	/*
	 *  If there are no buffered blocks, allocate a block
	 *  for the qproducer/qwrite to fill.  This is
	 *  optimistic and and we will
	 *  sometimes be wrong: after locking we may either
	 *  have to throw away or allocate one.
	 *
	 *  We hope to replace the allocb with a kmap later on.
	 */
retry:
	if(q->bfirst == 0)
		b = allocb(len);

	/* sync with qwrite/qproduce */
	x = splhi();
	lock(q);

	bb = q->bfirst;
	if(bb == 0){
		if(b == 0){
			/* we guessed wrong, drop the locks and try again */
			unlock(q);
			splx(x);
			qrtoofew++;
			goto retry;
		}

		/* add ourselves to the list of readers */
		if(q->rfirst)
			q->rlast->next = b;
		else
			q->rfirst = b;
		q->rlast = b;
		unlock(q);
		splx(x);
		qunlock(&q->rlock);
		poperror();

		if(waserror()){
			/* on error, unlink us from the chain */
			x = splhi();
			lock(q);
			l = &q->rfirst;
			for(bb = q->rfirst; bb; bb = bb->next){
				if(b == bb){
					*l = bb->next;
					break;
				} else
					l = &bb->next;
			}
			unlock(q);
			splx(x);
			free(b);
			nexterror();
		}

		/* wait for the producer */
		sleep(&b->r, bfilled, b);
		n = BLEN(b);
		memmove(p, b->rp, n);
		poperror();
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

	/* do this outside of the lock(q)! */
	memmove(p, bb->rp, n);
	bb->rp += n;

	/* free it or put it back on the queue */
	if(bb->rp >= bb->wp || (q->state&Qmsg))
		free(bb);
	else {
		x = splhi();
		lock(q);
		bb->next = q->bfirst;
		q->bfirst = bb;
		unlock(q);
		splx(x);
	}

	poperror();
	qunlock(&q->rlock);
	if(b){
		qrtoomany++;
		free(b);
	}
	return n;
}

ulong qwtoomany;
ulong qwtoofew;

static long
qwrite0(Queue *q, char *p, int len, Block *b)
{
	Block *bb;
	int x, n, sofar;

	/* sync with qconsume/qread */
	x = splhi();
	lock(q);

	sofar = 0;
	while(bb = q->rfirst){
		/* hand to waiting receiver */
		q->rfirst = bb->next;
		unlock(q);
		splx(x);

		n = bb->lim - bb->wp;
		if(n > len-sofar)
			n = len - sofar;
		memmove(bb->wp, p+sofar, n);
		bb->wp += n;
		bb->flag |= Bfilled;
		wakeup(&bb->r);

		sofar += n;
		if(sofar == len){
			if(b){
				free(b);	/* we were wrong to allocate */
				qwtoomany++;
			}
			return len;
		}
	}

	/* buffer what ever is left */
	if(b == 0){
		/* we should have alloc'd, return to qwrite and have it do it */
		unlock(q);
		splx(x);
		qwtoofew++;
		return sofar;
	}
	b->rp += sofar;

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

static int
qnotfull(void *a)
{
	Queue *q = a;

	return q->len < q->limit;
}

long
qwrite(Queue *q, char *p, int len)
{
	int n, i;
	Block *b;

	/*
	 *  If there are no readers, grab a buffer and copy
	 *  into it before locking anything down.  This
	 *  provides the highest concurrency but we will
	 *  sometimes be wrong: after locking we may either
	 *  have to throw away or allocate one.
	 */
	if(q->rfirst == 0){
		b = allocb(len);
		memmove(b->wp, p, len);
		b->wp += len;
	} else
		b = 0;

	/* ensure atomic writes */
	qlock(&q->wlock);
	if(waserror()){
		qunlock(&q->wlock);
		nexterror();
	}

	/* flow control */
	sleep(&q->r, qnotfull, q);

	n = qwrite0(q, p, len, b);
	if(n != len){
		/* no readers and we need a buffer */
		i = len - n;
		b = allocb(i);
		memmove(b->wp, p + n, i);
		b->wp += n;
		n += qwrite0(q, p + n, i, b);
	}

	qunlock(&q->wlock);
	poperror();

	return n;
}
