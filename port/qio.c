#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

struct
{
	Lock;
	ulong	bytes;
} ialloc;

static int debuging;

#define QDEBUG if(0)

/*
 *  IO queues
 */
typedef struct Queue	Queue;

struct Queue
{
	Lock;

	Block*	bfirst;		/* buffer */
	Block*	blast;

	int	len;		/* bytes in queue */
	int	limit;		/* max bytes in queue */
	int	inilim;		/* initial limit */
	int	state;
	int	noblock;	/* true if writes return immediately when q full */
	int	eof;		/* number of eofs read by user */

	void	(*kick)(void*);	/* restart output */
	void*	arg;		/* argument to kick */

	QLock	rlock;		/* mutex for reading processes */
	Rendez	rr;		/* process waiting to read */
	QLock	wlock;		/* mutex for writing processes */
	Rendez	wr;		/* process waiting to write */
};

enum
{
	/* Queue.state */	
	Qstarve		= (1<<0),	/* consumer starved */
	Qmsg		= (1<<1),	/* message stream */
	Qclosed		= (1<<2),
	Qflow		= (1<<3),
};

void
checkb(Block *b, char *msg)
{
	if(b->base > b->lim)
		panic("checkb 0 %s %lux %lux", msg, b->base, b->lim);
	if(b->rp < b->base)
		panic("checkb 1 %s %lux %lux", msg, b->base, b->rp);
	if(b->wp < b->base)
		panic("checkb 2 %s %lux %lux", msg, b->base, b->wp);
	if(b->rp > b->lim)
		panic("checkb 3 %s %lux %lux", msg, b->rp, b->lim);
	if(b->wp > b->lim)
		panic("checkb 4 %s %lux %lux", msg, b->wp, b->lim);
}

/*
 *  interrupt time allocation
 */
Block*
iallocb(int size)
{
	Block *b;
	ulong addr;

	size = sizeof(Block) + size + (BY2V-1);
	if(ialloc.bytes > conf.ialloc){
		iprint("iallocb: limited %d/%d\n", ialloc.bytes, conf.ialloc);
		return 0;
	}
	b = mallocz(size, 0);
	if(b == 0){
		iprint("iallocb: no memory %d/%d\n", ialloc.bytes, conf.ialloc);
		return 0;
	}
	memset(b, 0, sizeof(Block));

	addr = (ulong)b;
	addr = ROUND(addr + sizeof(Block), BY2V);
	b->base = (uchar*)addr;
	b->rp = b->base;
	b->wp = b->base;
	b->lim = ((uchar*)b) + size;
	b->flag = BINTR;

	ilock(&ialloc);
	ialloc.bytes += b->lim - b->base;
	iunlock(&ialloc);

	return b;
}

void
freeb(Block *b)
{
	/*
	 * drivers which perform non cache coherent DMA manage their own buffer 
	 * pool of uncached buffers and provide their own free routine.
	 */
	if(b->free) {
		b->free(b);
		return;
	}
	if(b->flag & BINTR) {
		ilock(&ialloc);
		ialloc.bytes -= b->lim - b->base;
		iunlock(&ialloc);
	}

	/* poison the block in case someone is still holding onto it */
	b->next = (void*)0xdeadbabe;
	b->rp = (void*)0xdeadbabe;
	b->wp = (void*)0xdeadbabe;
	b->lim = (void*)0xdeadbabe;
	b->base = (void*)0xdeadbabe;

	free(b);
}

void
ixsummary(void)
{
	debuging ^= 1;
	print("ialloc %d/%d %d\n", ialloc.bytes, conf.ialloc, debuging);
}

/*
 *  allocate queues and blocks (round data base address to 64 bit boundary)
 */
Block*
allocb(int size)
{
	Block *b;
	ulong addr;

	size = sizeof(Block) + size + (BY2V-1);
	b = mallocz(size, 0);
	if(b == 0)
		exhausted("Blocks");
	memset(b, 0, sizeof(Block));

	addr = (ulong)b;
	addr = ROUND(addr + sizeof(Block), BY2V);
	b->base = (uchar*)addr;
	b->rp = b->base;
	b->wp = b->base;
	b->lim = ((uchar*)b) + size;

	return b;
}

/*
 *  Interrupt level copy out of a queue, return # bytes copied.
 */
int
qconsume(Queue *q, void *vp, int len)
{
	Block *b;
	int n, dowakeup;
	uchar *p = vp;

	/* sync with qwrite */
	lock(q);

	for(;;) {
		b = q->bfirst;
		if(b == 0){
			q->state |= Qstarve;
			unlock(q);
			return -1;
		}
		QDEBUG checkb(b, "qconsume 1");

		n = BLEN(b);
		if(n > 0)
			break;
		q->bfirst = b->next;
		freeb(b);
	};

	if(n < len)
		len = n;
	memmove(p, b->rp, len);
	if((q->state & Qmsg) || len == n)
		q->bfirst = b->next;
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

	QDEBUG checkb(b, "qconsume 2");
	/* discard the block if we're done with it */
	if((q->state & Qmsg) || len == n)
		freeb(b);
	return len;
}

int
qpass(Queue *q, Block *b)
{
	int len, dowakeup;

	len = BLEN(b);
	/* sync with qread */
	dowakeup = 0;
	ilock(q);

	/* save in buffer */
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += len;
	QDEBUG checkb(b, "qpass");

	if(q->len >= q->limit/2)
		q->state |= Qflow;

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	}
	iunlock(q);

	if(dowakeup)
		wakeup(&q->rr);

	return len;
}

int
qproduce(Queue *q, void *vp, int len)
{
	Block *b;
	int dowakeup;
	uchar *p = vp;

	/* sync with qread */
	dowakeup = 0;
	lock(q);

	/* no waiting receivers, room in buffer? */
	if(q->len >= q->limit){
		q->state |= Qflow;
		unlock(q);
		return -1;
	}

	/* save in buffer */
	b = iallocb(len);
	if(b == 0){
		unlock(q);
		return 0;
	}
	memmove(b->wp, p, len);
	b->wp += len;
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += len;
	QDEBUG checkb(b, "qproduce");

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	}
	unlock(q);

	if(dowakeup)
		wakeup(&q->rr);

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

	q->limit = q->inilim = limit;
	q->kick = kick;
	q->arg = arg;
	q->state = msg ? Qmsg : 0;
	q->state |= Qstarve;
	q->eof = 0;

	return q;
}

static int
notempty(void *a)
{
	Queue *q = a;

	return (q->state & Qclosed) || q->bfirst != 0;
}

/*
 *  get next block from a queue (up to a limit)
 */
Block*
qbread(Queue *q, int len)
{
	Block *b, *nb;
	int n, dowakeup;

	qlock(&q->rlock);
	if(waserror()){
		qunlock(&q->rlock);
		nexterror();
	}

	/* wait for data */
	for(;;){
		/* sync with qwrite/qproduce */
		ilock(q);

		b = q->bfirst;
		if(b)
			break;

		if(q->state & Qclosed){
			iunlock(q);
			poperror();
			qunlock(&q->rlock);
			if(++q->eof > 3)
				error(Ehungup);
			return 0;
		}

		q->state |= Qstarve;	/* flag requesting producer to wake me */
		iunlock(q);
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

	/* split block if its too big and this is not a message oriented queue */
	nb = b;
	if(n > len){
		if((q->state&Qmsg) == 0){
			n -= len;
			b = allocb(n);
			memmove(b->wp, nb->rp+len, n);
			b->wp += n;

			b->next = q->bfirst;
			if(q->bfirst == 0)
				q->blast = b;
			q->bfirst = b;
			q->len += n;
		}
		nb->wp = nb->rp + len;
	}

	iunlock(q);

	/* wakeup flow controlled writers */
	if(dowakeup){
		if(q->kick)
			(*q->kick)(q->arg);
		wakeup(&q->wr);
	}

	poperror();
	qunlock(&q->rlock);
	return nb;
}

/*
 *  read a queue.  if no data is queued, post a Block
 *  and wait on its Rendez.
 */
long
qread(Queue *q, void *vp, int len)
{
	Block *b;

	b = qbread(q, len);
	if(b == 0)
		return 0;

	len = BLEN(b);
	memmove(vp, b->rp, len);
	freeb(b);
	return len;
}

static int
qnotfull(void *a)
{
	Queue *q = a;

	return q->len < q->limit || (q->state & Qclosed);
}

/*
 *  add a block to a queue obeying flow control
 */
long
qbwrite(Queue *q, Block *b)
{
	int n, dowakeup;

	dowakeup = 0;
	n = BLEN(b);

	if(waserror()){
		qunlock(&q->wlock);
		nexterror();
	}
	qlock(&q->wlock);

	/* flow control */
	while(!qnotfull(q)){
		if(q->noblock){
			freeb(b);
			qunlock(&q->wlock);
			poperror();
			return n;
		}
		q->state |= Qflow;
		sleep(&q->wr, qnotfull, q);
	}

	ilock(q);

	if(q->state & Qclosed){
		iunlock(q);
		error(Ehungup);
	}

	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	q->len += n;

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	}

	iunlock(q);

	if(dowakeup){
		if(q->kick)
			(*q->kick)(q->arg);
		wakeup(&q->rr);
	}

	qunlock(&q->wlock);
	poperror();
	return n;
}

/*
 *  write to a queue.  only 128k at a time is atomic.
 */
long
qwrite(Queue *q, void *vp, int len)
{
	int n, sofar;
	Block *b;
	uchar *p = vp;

	QDEBUG if((getstatus()&IE) == 0)
		print("qwrite hi %lux\n", getcallerpc(q));

	sofar = 0;
	do {
		n = len-sofar;
		if(n > 128*1024)
			n = 128*1024;

		b = allocb(n);
		if(waserror()){
			freeb(b);
			nexterror();
		}
		memmove(b->wp, p+sofar, n);
		poperror();
		b->wp += n;

		qbwrite(q, b);

		sofar += n;
	} while(sofar < len && (q->state & Qmsg) == 0);

	return len;
}

/*
 *  used by print() to write to a queue.  Since we may be splhi or not in
 *  a process, don't qlock.
 */
int
qiwrite(Queue *q, void *vp, int len)
{
	int n, sofar, dowakeup;
	Block *b;
	uchar *p = vp;

	dowakeup = 0;

	sofar = 0;
	do {
		n = len-sofar;
		if(n > 128*1024)
			n = 128*1024;

		b = allocb(n);
		memmove(b->wp, p+sofar, n);
		b->wp += n;

		ilock(q);

		QDEBUG checkb(b, "qiwrite");
		if(q->bfirst)
			q->blast->next = b;
		else
			q->bfirst = b;
		q->blast = b;
		q->len += n;

		if(q->state & Qstarve){
			q->state &= ~Qstarve;
			dowakeup = 1;
		}

		iunlock(q);

		if(dowakeup){
			if(q->kick)
				(*q->kick)(q->arg);
			wakeup(&q->rr);
		}

		sofar += n;
	} while(sofar < len && (q->state & Qmsg) == 0);

	return len;
}

/*
 *  Mark a queue as closed.  No further IO is permitted.
 *  All blocks are released.
 */
void
qclose(Queue *q)
{
	Block *b, *bfirst;

	/* mark it */
	ilock(q);
	q->state |= Qclosed;
	bfirst = q->bfirst;
	q->bfirst = 0;
	q->len = 0;
	q->noblock = 0;
	iunlock(q);

	/* free queued blocks */
	while(bfirst){
		b = bfirst->next;
		freeb(bfirst);
		bfirst = b;
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
	/* mark it */
	ilock(q);
	q->state |= Qclosed;
	iunlock(q);

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
	q->eof = 0;
	q->limit = q->inilim;
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
 * return space remaining before flow control
 */
int
qwindow(Queue *q)
{
	int l;

	l = q->limit - q->len;
	if(l < 0)
		l = 0;
	return l;
}

/*
 *  return true if we can read without blocking
 */
int
qcanread(Queue *q)
{
	return q->bfirst!=0;
}

/*
 *  change queue limit
 */
void
qsetlimit(Queue *q, int limit)
{
	q->limit = limit;
}

/*
 *  set blocking/nonblocking
 */
void
qnoblock(Queue *q, int onoff)
{
	q->noblock = onoff;
}

/*
 *  flush the output queue
 */
void
qflush(Queue *q)
{
	Block *b, *bfirst;

	/* mark it */
	ilock(q);
	bfirst = q->bfirst;
	q->bfirst = 0;
	q->len = 0;
	iunlock(q);

	/* free queued blocks */
	while(bfirst){
		b = bfirst->next;
		freeb(bfirst);
		bfirst = b;
	}

	/* wake up readers/writers */
	wakeup(&q->wr);
}

int
qstate(Queue *q)
{
	return q->state;
}
