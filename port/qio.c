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

static ulong padblockcnt;
static ulong concatblockcnt;
static ulong pullupblockcnt;
static ulong copyblockcnt;
static ulong consumecnt;
static ulong producecnt;
static ulong qcopycnt;

static int debugging;

#define QDEBUG	if(0)

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

	char	err[ERRLEN];
};

enum
{
	/* Queue.state */
	Qstarve		= (1<<0),	/* consumer starved */
	Qmsg		= (1<<1),	/* message stream */
	Qclosed		= (1<<2),
	Qflow		= (1<<3),

	Hdrspc		= 64,		/* leave room for high-level headers */

	Bdead		= 0x51494F42,	/* "QIOB" */
};

void
checkb(Block *b, char *msg)
{
	void *dead = (void*)Bdead;

	if(b == dead)
		panic("checkb b %s %lux", msg, b);
	if(b->base == dead || b->lim == dead || b->next == dead
	  || b->rp == dead || b->wp == dead){
		print("checkb: base 0x%8.8luX lim 0x%8.8luX next 0x%8.8luX\n",
			b->base, b->lim, b->next);
		print("checkb: rp 0x%8.8luX wp 0x%8.8luX\n", b->rp, b->wp);
		panic("checkb dead: %s\n", msg);
	}

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

void
ixsummary(void)
{
	debugging ^= 1;
	print("ialloc %d/%d %d\n", ialloc.bytes, conf.ialloc, debugging);
	print("pad %lud, concat %lud, pullup %lud, copy %lud\n",
		padblockcnt, concatblockcnt, pullupblockcnt, copyblockcnt);
	print("consume %lud, produce %lud, qcopy %lud\n",
		consumecnt, producecnt, qcopycnt);
}

/*
 *  allocate blocks (round data base address to 64 bit boundary).
 *  if mallocz gives us more than we asked for, leave room at the front
 *  for header.
 */
Block*
allocb(int size)
{
	Block *b;
	ulong addr;
	int n;

	n = sizeof(Block) + size + (BY2V-1);
	b = mallocz(n+Hdrspc, 0);
	if(b == 0)
		exhausted("Blocks");
	memset(b, 0, sizeof(Block));

	addr = (ulong)b;
	addr = ROUND(addr + sizeof(Block), BY2V);
	b->base = (uchar*)addr;
	b->lim = ((uchar*)b) + msize(b);
	b->rp = b->base;
	n = b->lim - b->base - size;
	b->rp += n & ~(BY2V-1);
	b->wp = b->rp;

	return b;
}

/*
 *  interrupt time allocation
 */
Block*
iallocb(int size)
{
	Block *b;
	ulong addr;
	int n;

	if(ialloc.bytes > conf.ialloc){
		iprint("iallocb: limited %d/%d\n", ialloc.bytes, conf.ialloc);
		return 0;
	}

	n = sizeof(Block) + size + (BY2V-1);
	b = mallocz(n+Hdrspc, 0);
	if(b == nil){
		iprint("iallocb: no memory %d/%d\n", ialloc.bytes, conf.ialloc);
		return nil;
	}
	memset(b, 0, sizeof(Block));

	addr = (ulong)b;
	addr = ROUND(addr + sizeof(Block), BY2V);
	b->base = (uchar*)addr;
	b->lim = ((uchar*)b) + msize(b);
	b->rp = b->base;
	n = b->lim - b->base - size;
	b->rp += n & ~(BY2V-1);
	b->wp = b->rp;

	b->flag = BINTR;

	ilock(&ialloc);
	ialloc.bytes += b->lim - b->base;
	iunlock(&ialloc);

	return b;
}

void
freeb(Block *b)
{
	void *dead = (void*)Bdead;

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
	b->next = dead;
	b->rp = dead;
	b->wp = dead;
	b->lim = dead;
	b->base = dead;

	free(b);
}

/*
 *  free a list of blocks
 */
void
freeblist(Block *b)
{
	Block *next;

	for(; b != 0; b = next){
		next = b->next;
		b->next = 0;
		freeb(b);
	}
}

/*
 *  pad a block to the front (or the back if size is negative)
 */
Block*
padblock(Block *bp, int size)
{
	int n;
	Block *nbp;

	QDEBUG checkb(bp, "padblock 1");
	if(size >= 0){
		if(bp->rp - bp->base >= size){
			bp->rp -= size;
			return bp;
		}

		if(bp->next)
			panic("padblock 0x%uX", getcallerpc(bp));
		n = BLEN(bp);
		padblockcnt++;
		nbp = allocb(size+n);
		nbp->rp += size;
		nbp->wp = nbp->rp;
		memmove(nbp->wp, bp->rp, n);
		nbp->wp += n;
		freeb(bp);
		nbp->rp -= size;
	} else {
		size = -size;

		if(bp->lim - bp->wp >= size)
			return bp;

		if(bp->next)
			panic("padblock 0x%uX", getcallerpc(bp));
		n = BLEN(bp);
		padblockcnt++;
		nbp = allocb(size+n);
		memmove(nbp->wp, bp->rp, n);
		nbp->wp += n;
		freeb(bp);
	}
	QDEBUG checkb(nbp, "padblock 1");
	return nbp;
}

/*
 *  return count of bytes in a string of blocks
 */
int
blocklen(Block *bp)
{
	int len;

	len = 0;
	while(bp) {
		len += BLEN(bp);
		bp = bp->next;
	}
	return len;
}

/*
 *  copy the  string of blocks into
 *  a single block and free the string
 */
Block*
concatblock(Block *bp)
{
	int len;
	Block *nb, *f;

	if(bp->next == 0)
		return bp;

	nb = allocb(blocklen(bp));
	for(f = bp; f; f = f->next) {
		len = BLEN(f);
		memmove(nb->wp, f->rp, len);
		nb->wp += len;
	}
	concatblockcnt += BLEN(nb);
	freeblist(bp);
	QDEBUG checkb(nb, "concatblock 1");
	return nb;
}

/*
 *  make sure the first block has at least n bytes
 */
Block*
pullupblock(Block *bp, int n)
{
	int i;
	Block *nbp;

	/*
	 *  this should almost always be true, it's
	 *  just to avoid every caller checking.
	 */
	if(BLEN(bp) >= n)
		return bp;

	/*
	 *  if not enough room in the first block,
	 *  add another to the front of the list.
	 */
	if(bp->lim - bp->rp < n){
		nbp = allocb(n);
		nbp->next = bp;
		bp = nbp;
	}

	/*
	 *  copy bytes from the trailing blocks into the first
	 */
	n -= BLEN(bp);
	while(nbp = bp->next){
		i = BLEN(nbp);
		if(i > n) {
			memmove(bp->wp, nbp->rp, n);
			pullupblockcnt++;
			bp->wp += n;
			nbp->rp += n;
			QDEBUG checkb(bp, "pullupblock 1");
			return bp;
		}
		else {
			memmove(bp->wp, nbp->rp, i);
			pullupblockcnt++;
			bp->wp += i;
			bp->next = nbp->next;
			nbp->next = 0;
			freeb(nbp);
			n -= i;
			if(n == 0){
				QDEBUG checkb(bp, "pullupblock 2");
				return bp;
			}
		}
	}
	freeb(bp);
	return 0;
}

/*
 *  trim to len bytes starting at offset
 */
Block *
trimblock(Block *bp, int offset, int len)
{
	ulong l;
	Block *nb, *startb;

	QDEBUG checkb(bp, "trimblock 1");
	if(blocklen(bp) < offset+len) {
		freeblist(bp);
		return nil;
	}

	while((l = BLEN(bp)) < offset) {
		offset -= l;
		nb = bp->next;
		bp->next = nil;
		freeb(bp);
		bp = nb;
	}

	startb = bp;
	bp->rp += offset;

	while((l = BLEN(bp)) < len) {
		len -= l;
		bp = bp->next;
	}

	bp->wp -= (BLEN(bp) - len);

	if(bp->next) {
		freeblist(bp->next);
		bp->next = nil;
	}

	return startb;
}

/*
 *  copy 'count' bytes into a new block
 */
Block*
copyblock(Block *bp, int count)
{
	int l;
	Block *nbp;

	QDEBUG checkb(bp, "copyblock 0");
	nbp = allocb(count);
	for(; count > 0 && bp != 0; bp = bp->next){
		l = BLEN(bp);
		if(l > count)
			l = count;
		memmove(nbp->wp, bp->rp, l);
		nbp->wp += l;
		count -= l;
	}
	if(count > 0){
		memset(nbp->wp, 0, count);
		nbp->wp += count;
	}
	copyblockcnt++;
	QDEBUG checkb(nbp, "copyblock 1");

	return nbp;
}

Block*
adjustblock(Block* bp, int len)
{
	int n;
	Block *nbp;

	if(len < 0){
		freeb(bp);
		return nil;
	}

	if(bp->rp+len > bp->lim){
		nbp = copyblock(bp, len);
		freeblist(bp);
		QDEBUG checkb(nbp, "adjustblock 1");

		return nbp;
	}

	n = BLEN(bp);
	if(len > n)
		memset(bp->wp, 0, len-n);
	bp->wp = bp->rp+len;
	QDEBUG checkb(bp, "adjustblock 2");

	return bp;
}


/*
 *  throw away up to count bytes from a
 *  list of blocks.  Return count of bytes
 *  thrown away.
 */
int
pullblock(Block **bph, int count)
{
	Block *bp;
	int n, bytes;

	bytes = 0;
	if(bph == nil)
		return 0;

	while(*bph != nil && count != 0) {
		bp = *bph;
		n = BLEN(bp);
		if(count < n)
			n = count;
		bytes += n;
		count -= n;
		bp->rp += n;
		QDEBUG checkb(bp, "pullblock ");
		if(BLEN(bp) == 0) {
			*bph = bp->next;
			bp->next = nil;
			freeb(bp);
		}
	}
	return bytes;
}

/*
 *  get next block from a queue, return null if nothing there
 */
Block*
qget(Queue *q)
{
	int dowakeup;
	Block *b;

	/* sync with qwrite */
	ilock(q);

	b = q->bfirst;
	if(b == 0){
		q->state |= Qstarve;
		iunlock(q);
		return 0;
	}
	q->bfirst = b->next;
	b->next = 0;
	q->len -= BLEN(b);
	QDEBUG checkb(b, "qget");

	/* if writer flow controlled, restart */
	if((q->state & Qflow) && q->len < q->limit/2){
		q->state &= ~Qflow;
		dowakeup = 1;
	} else
		dowakeup = 0;

	iunlock(q);

	if(dowakeup)
		wakeup(&q->wr);

	return b;
}

/*
 *  throw away the next 'len' bytes in the queue
 */
void
qdiscard(Queue *q, int len)
{
	Block *b;
	int dowakeup, n, sofar;

	ilock(q);
	for(sofar = 0; sofar < len; sofar += n){
		b = q->bfirst;
		if(b == nil)
			break;
		QDEBUG checkb(b, "qdiscard");
		n = BLEN(b);
		if(n <= len - sofar){
			q->bfirst = b->next;
			b->next = 0;
			freeb(b);
		} else {
			n = len - sofar;
			b->rp += n;
		}
		q->len -= n;
	}

	/* if writer flow controlled, restart */
	if((q->state & Qflow) && q->len < q->limit/2){
		q->state &= ~Qflow;
		dowakeup = 1;
	} else
		dowakeup = 0;

	iunlock(q);

	if(dowakeup)
		wakeup(&q->wr);
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
	ilock(q);

	for(;;) {
		b = q->bfirst;
		if(b == 0){
			q->state |= Qstarve;
			iunlock(q);
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
	consumecnt += n;
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

	iunlock(q);

	if(dowakeup)
		wakeup(&q->wr);

	QDEBUG checkb(b, "qconsume 2");
	/* discard the block if we're done with it */
	if((q->state & Qmsg) || len == n){
		b->next = 0;
		freeb(b);
	}
	return len;
}

int
qpass(Queue *q, Block *b)
{
	int len, dowakeup;

	/* sync with qread */
	dowakeup = 0;
	ilock(q);
	if(q->len >= q->limit){
		freeb(b);
		iunlock(q);
		return -1;
	}

	/* add buffer to queue */
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	len = BLEN(b);
	QDEBUG checkb(b, "qpass");
	while(b->next){
		b = b->next;
		QDEBUG checkb(b, "qpass");
		len += BLEN(b);
	}
	q->blast = b;
	q->len += len;

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
	ilock(q);

	/* no waiting receivers, room in buffer? */
	if(q->len >= q->limit){
		q->state |= Qflow;
		iunlock(q);
		return -1;
	}

	/* save in buffer */
	b = iallocb(len);
	if(b == 0){
		iunlock(q);
		return 0;
	}
	memmove(b->wp, p, len);
	producecnt += len;
	b->wp += len;
	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	/* b->next = 0; done by iallocb() */
	q->len += len;
	QDEBUG checkb(b, "qproduce");

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	}
	iunlock(q);

	if(dowakeup)
		wakeup(&q->rr);

	return len;
}

/*
 *  copy from offset in the queue
 */
Block*
qcopy(Queue *q, int len, ulong offset)
{
	int sofar;
	int n;
	Block *b, *nb;
	uchar *p;

	nb = allocb(len);

	ilock(q);

	/* go to offset */
	b = q->bfirst;
	for(sofar = 0; ; sofar += n){
		if(b == nil){
			iunlock(q);
			return nb;
		}
		n = BLEN(b);
		if(sofar + n > offset){
			p = b->rp + offset - sofar;
			n -= offset - sofar;
			break;
		}
		QDEBUG checkb(b, "qcopy");
		b = b->next;
	}

	/* copy bytes from there */
	for(sofar = 0; sofar < len;){
		if(n > len - sofar)
			n = len - sofar;
		memmove(nb->wp, p, n);
		qcopycnt += n;
		sofar += n;
		nb->wp += n;
		b = b->next;
		if(b == nil)
			break;
		n = BLEN(b);
		p = b->rp;
	}
	iunlock(q);

	return nb;
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

	ilock(q);
	q->limit = q->inilim = limit;
	q->kick = kick;
	q->arg = arg;
	q->state = msg ? Qmsg : 0;
	q->state |= Qstarve;
	q->eof = 0;
	iunlock(q);

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
		if(b){
			QDEBUG checkb(b, "qbread 0");
			break;
		}

		if(q->state & Qclosed){
			iunlock(q);
			poperror();
			qunlock(&q->rlock);
			if(++q->eof > 3)
				error(q->err);
			return 0;
		}

		q->state |= Qstarve;	/* flag requesting producer to wake me */
		iunlock(q);
		sleep(&q->rr, notempty, q);
	}

	/* remove a buffered block */
	q->bfirst = b->next;
	b->next = 0;
	n = BLEN(b);
	q->len -= n;
	QDEBUG checkb(b, "qbread 1");

	/* if writer flow controlled, restart */
	if((q->state & Qflow) && q->len < q->limit/2){
		q->state &= ~Qflow;
		dowakeup = 1;
	} else
		dowakeup = 0;

	/* split block if it's too big and this is not a message-oriented queue */
	nb = b;
	if(n > len){
		if((q->state&Qmsg) == 0){
			iunlock(q);

			n -= len;
			b = allocb(n);
			memmove(b->wp, nb->rp+len, n);
			b->wp += n;

			ilock(q);
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
			q->kick(q->arg);
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

	qlock(&q->wlock);
	if(waserror()){
		qunlock(&q->wlock);
		nexterror();
	}

	/* flow control */
	for(;;){
		ilock(q);

		if(q->state & Qclosed){
			iunlock(q);
			freeb(b);
			error(q->err);
		}

		if(q->len < q->limit)
			break;

		if(q->noblock){
			iunlock(q);
			freeb(b);
			qunlock(&q->wlock);
			poperror();
			return n;
		}

		q->state |= Qflow;
		iunlock(q);
		sleep(&q->wr, qnotfull, q);
	}

	if(q->bfirst)
		q->blast->next = b;
	else
		q->bfirst = b;
	q->blast = b;
	b->next = 0;
	q->len += n;
	QDEBUG checkb(b, "qbwrite");

	if(q->state & Qstarve){
		q->state &= ~Qstarve;
		dowakeup = 1;
	}

	iunlock(q);

	if(dowakeup){
		if(q->kick)
			q->kick(q->arg);
		wakeup(&q->rr);
	}

	qunlock(&q->wlock);
	poperror();
	return n;
}

/*
 *  write to a queue.  only 128k at a time is atomic.
 */
int
qwrite(Queue *q, void *vp, int len)
{
	int n, sofar;
	Block *b;
	uchar *p = vp;

	QDEBUG if(islo() == 0)
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
				q->kick(q->arg);
			wakeup(&q->rr);
		}

		sofar += n;
	} while(sofar < len && (q->state & Qmsg) == 0);

	return len;
}

/*
 *  be extremely careful when calling this,
 *  as there is no reference accounting
 */
void
qfree(Queue *q)
{
	qclose(q);
	free(q);
}

/*
 *  Mark a queue as closed.  No further IO is permitted.
 *  All blocks are released.
 */
void
qclose(Queue *q)
{
	Block *bfirst;

	if(q == nil)
		return;

	/* mark it */
	ilock(q);
	q->state |= Qclosed;
	q->state &= ~(Qflow|Qstarve);
	strcpy(q->err, Ehungup);
	bfirst = q->bfirst;
	q->bfirst = 0;
	q->len = 0;
	q->noblock = 0;
	iunlock(q);

	/* free queued blocks */
	freeblist(bfirst);

	/* wake up readers/writers */
	wakeup(&q->rr);
	wakeup(&q->wr);
}

/*
 *  Mark a queue as closed.  Wakeup any readers.  Don't remove queued
 *  blocks.
 */
void
qhangup(Queue *q, char *msg)
{
	/* mark it */
	ilock(q);
	q->state |= Qclosed;
	if(msg == 0 || *msg == 0)
		strcpy(q->err, Ehungup);
	else
		strncpy(q->err, msg, ERRLEN-1);
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
	ilock(q);
	q->state &= ~Qclosed;
	q->state |= Qstarve;
	q->eof = 0;
	q->limit = q->inilim;
	iunlock(q);
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
	Block *bfirst;

	/* mark it */
	ilock(q);
	bfirst = q->bfirst;
	q->bfirst = 0;
	q->len = 0;
	iunlock(q);

	/* free queued blocks */
	freeblist(bfirst);

	/* wake up readers/writers */
	wakeup(&q->wr);
}

int
qfull(Queue *q)
{
	return q->state & Qflow;
}

int
qstate(Queue *q)
{
	return q->state;
}
