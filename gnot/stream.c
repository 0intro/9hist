#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"devtab.h"

static void stputq(Queue*, Block*);
Qinfo procinfo = { stputq, nullput, 0, 0, "process" } ;
/*extern Qinfo noetherinfo;	*/

static Qinfo *lds[] = {
/*	&noetherinfo,	*/
	0
};

enum {
	Nclass=4,
};

/*
 *  All stream structures are ialloc'd at boot time
 */
Stream *slist;
Queue *qlist;
Block *blist;
static Lock garbagelock;

/*
 *  The block classes.  There are Nclass block sizes, each with its own free list.
 *  All are ialloced at qinit() time.
 */
typedef struct {
	int	size;
	Queue;
} Bclass;
Bclass bclass[Nclass]={
	{ 0 },
	{ 64 },
	{ 512 },
	{ 4096 },
};

/*
 *  Allocate streams, queues, and blocks.  Allocate n block classes with
 *	1/2(m+1) to class m < n-1
 *	1/2(n-1) to class n-1
 */
void
streaminit(void)
{
	int class, i, n;
	Block *bp;
	Bclass *bcp;

	slist = (Stream *)ialloc(conf.nstream * sizeof(Stream), 0);
	qlist = (Queue *)ialloc(conf.nqueue * sizeof(Queue), 0);
	blist = (Block *)ialloc(conf.nblock * sizeof(Block), 0);
	bp = blist;
	n = conf.nblock;
	for(class = 0; class < Nclass; class++){
		if(class < Nclass-1)
			n = n/2;
		bcp = &bclass[class];
		for(i = 0; i < n; i++) {
			if(bcp->size)
				bp->base = (uchar *)ialloc(bcp->size, 0);
			bp->lim = bp->base + bcp->size;
			bp->flags = class;
			freeb(bp);
			bp++;
		}
	}
}


/*
 *  allocate a block
 */
static int
isblock(void *arg)
{
	Bclass *bcp;

	bcp = (Bclass *)arg;
	return bcp->first!=0;
}
Block *
allocb(ulong size)
{
	Block *bp;
	Bclass *bcp;
	int i;


	/*
	 *  map size to class
	 */
	for(bcp=bclass; bcp->size<size && bcp<&bclass[Nclass-1]; bcp++)
		;

	/*
	 *  look for a free block, garbage collect if there are none
	 */
	lock(bcp);
	while(bcp->first == 0){
		unlock(bcp);
		print("waiting for blocks\n");
		sleep(&bcp->r, isblock, (void *)bcp);
		lock(bcp);
	}
	bp = bcp->first;
	bcp->first = bp->next;
	if(bcp->first == 0)
		bcp->last = 0;
	unlock(bcp);

	/*
	 *  return an empty block
	 */
	bp->rptr = bp->wptr = bp->base;
	bp->next = 0;
	bp->type = M_DATA;
	bp->flags &= S_CLASS;
	return bp;
}

/*
 *  Free a block.  Poison its pointers so that someone trying to access
 *  it after freeing will cause a dump.
 */
void
freeb(Block *bp)
{
	Bclass *bcp;

	bcp = &bclass[bp->flags & S_CLASS];
	bp->rptr = bp->wptr = 0;
	lock(bcp);
	if(bcp->first)
		bcp->last->next = bp;
	else
		bcp->first = bp;
	bcp->last = bp;
	bp->next = 0;
	wakeup(&bcp->r);
	unlock(bcp);
}

/*
 *  allocate a pair of queues.  flavor them with the requested put routines.
 *  the `QINUSE' flag on the read side is the only one used.
 */
static Queue *
allocq(Qinfo *qi)
{
	Queue *q, *wq;

	for(q=qlist; q<&qlist[conf.nqueue]; q++, q++) {
		if(q->flag == 0){
			if(canlock(q)){
				if(q->flag == 0)
					break;
				unlock(q);
			}
		}
	}

	if(q == &qlist[conf.nqueue]){
		print("no more queues\n");
		error(0, Enoqueue);
	}

	q->flag = QINUSE;
	q->r.p = 0;
	q->info = qi;
	q->put = qi->iput;
	wq = q->other = q + 1;

	wq->r.p = 0;
	wq->info = qi;
	wq->put = qi->oput;
	wq->other = q;

	unlock(q);

	return q;
}

/*
 *  free a queue
 */
static void
freeq(Queue *q)
{
	Block *bp;

	q = RD(q);
	while(bp = getq(q))
		freeb(bp);
	q = WR(q);
	while(bp = getq(q))
		freeb(bp);
	RD(q)->flag = 0;
}

/*
 *  push a queue onto a stream referenced by the proc side write q
 */
Queue *
pushq(Stream* s, Qinfo *qi)
{
	Queue *q;
	Queue *nq;

	q = RD(s->procq);

	/*
	 *  make the new queue
	 */
	nq = allocq(qi);

	/*
	 *  push
	 */
	RD(nq)->next = q;
	RD(WR(q)->next)->next = RD(nq);
	WR(nq)->next = WR(q)->next;
	WR(q)->next = WR(nq);

	if(qi->open)
		(*qi->open)(RD(nq), s);

	return WR(nq)->next;
}

/*
 *  pop off the top line discipline
 */
static void
popq(Stream *s)
{
	Queue *q;

	if(s->procq->next == WR(s->devq))
		error(0, Ebadld);
	q = s->procq->next;
	if(q->info->close)
		(*q->info->close)(RD(q));
	s->procq->next = q->next;
	RD(q->next)->next = RD(s->procq);
	freeq(q);
}

/*
 *  add a block (or list of blocks) to the end of a queue.  return true
 *  if one of the blocks contained a delimiter. 
 */
int
putq(Queue *q, Block *bp)
{
	int delim;

	delim = 0;
	lock(q);
	if(q->first)
		q->last->next = bp;
	else
		q->first = bp;
	q->len += bp->wptr - bp->rptr;
	delim = bp->flags & S_DELIM;
	while(bp->next) {
		bp = bp->next;
		q->len += bp->wptr - bp->rptr;
		delim |= bp->flags & S_DELIM;
	}
	q->last = bp;
	if(q->len >= Streamhi)
		q->flag |= QHIWAT;
	unlock(q);
	return delim;
}
int
putb(Blist *q, Block *bp)
{
	int delim;

	delim = 0;
	if(q->first)
		q->last->next = bp;
	else
		q->first = bp;
	q->len += bp->wptr - bp->rptr;
	delim = bp->flags & S_DELIM;
	while(bp->next) {
		bp = bp->next;
		q->len += bp->wptr - bp->rptr;
		delim |= bp->flags & S_DELIM;
	}
	q->last = bp;
	bp->next = 0;
	return delim;
}

/*
 *  add a block to the start of a queue 
 */
static void
putbq(Blist *q, Block *bp)
{
	lock(q);
	if(q->first)
		bp->next = q->first;
	else
		q->last = bp;
	q->first = bp;
	q->len += bp->wptr - bp->rptr;
	unlock(q);
}

/*
 *  remove the first block from a queue 
 */
Block *
getq(Queue *q)
{
	Block *bp;

	lock(q);
	bp = q->first;
	if(bp) {
		q->first = bp->next;
		if(q->first == 0)
			q->last = 0;
		q->len -= bp->wptr - bp->rptr;
		if((q->flag&QHIWAT) && q->len < Streamhi/2){
			wakeup(&q->other->next->other->r);
			q->flag &= ~QHIWAT;
		}
		bp->next = 0;
	}
	unlock(q);
	return bp;
}
Block *
getb(Blist *q)
{
	Block *bp;

	bp = q->first;
	if(bp) {
		q->first = bp->next;
		if(q->first == 0)
			q->last = 0;
		q->len -= bp->wptr - bp->rptr;
		bp->next = 0;
	}
	return bp;
}

/*
 *  put a block into the bit bucket
 */
void
nullput(Queue *q, Block *bp)
{
	freeb(bp);
	error(0, Ehungup);
}

/*
 *  find the info structure for line discipline 'name'
 */
static Qinfo *
qinfofind(char *name)
{
	Qinfo **qip;

	if(name == 0)
		error(0, Ebadld);
	for(qip = lds; *qip; qip++)
		if(strcmp((*qip)->name, name)==0)
			return *qip;
	error(0, Ebadld);
}

/*
 *  send a hangup up a stream
 */
static void
hangup(Stream *s)
{
	Block *bp;

	bp = allocb(0);
	bp->type = M_HANGUP;
	(*s->devq->put)(s->devq, bp);
}

/*
 *  parse a string and return a pointer to the second element if the 
 *  first matches name.  bp->rptr will be updated to point to the
 *  second element.
 *
 *  return 0 if no match.
 *
 *  it is assumed that the block data is null terminated.  streamwrite
 *  guarantees this.
 */
int
streamparse(char *name, Block *bp)
{
	int len;

	len = strlen(name);
	if(bp->wptr - bp->rptr < len)
		return 0;
	if(strncmp(name, (char *)bp->rptr, len)==0){
		if(bp->rptr[len] == ' ')
			bp->rptr += len+1;
		else if(bp->rptr[len])
			return 0;
		else
			bp->rptr += len;
		return 1;
	}
	return 0;
}

/*
 *  the per stream directory structure
 */
Dirtab streamdir[]={
	"data",		Sdataqid,	0,			0600,
	"ctl",		Sctlqid,	0,			0600,
};

/*
 *  A stream device consists of the contents of streamdir plus
 *  any directory supplied by the actual device.
 *
 *  values of s:
 * 	0 to ntab-1 apply to the auxiliary directory.
 *	ntab to ntab+Shighqid-Slowqid+1 apply to streamdir.
 */
int
streamgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Proc *p;
	char buf[NAMELEN];

	if(s < ntab)
		tab = &tab[s];
	else if(s < ntab + Shighqid - Slowqid + 1)
		tab = &streamdir[s - ntab];
	else
		return -1;

	devdir(c, STREAMQID(STREAMID(c->qid),tab->qid), tab->name, tab->length,
		tab->perm, dp);
	return 1;
}

/*
 *  create a new stream
 */
Stream *
streamnew(Chan *c, Qinfo *qi)
{
	Stream *s;
	Queue *q;

	/*
	 *  find a free stream struct
	 */
	for(s = slist; s < &slist[conf.nstream]; s++) {
		if(s->inuse == 0){
			if(canlock(s)){
				if(s->inuse == 0)
					break;
				unlock(s);
			}
		}
	}
	if(s == &slist[conf.nstream]){
		print("no more streams\n");
		error(0, Enostream);
	}
	if(waserror()){
		unlock(s);
		streamclose(c);
		nexterror();
	}

	/*
	 *  marry a stream and a channel
	 */
	if(c){
		c->stream = s;
		s->type = c->type;
		s->dev = c->dev;
		s->id = STREAMID(c->qid);
	} else
		s->type = -1;

	/*
 	 *  hang a device and process q off the stream
	 */
	s->inuse = 1;
	s->tag[0] = 0;
	q = allocq(&procinfo);
	s->procq = WR(q);
	q = allocq(qi);
	s->devq = RD(q);
	WR(s->procq)->next = WR(s->devq);
	RD(s->procq)->next = 0;
	RD(s->devq)->next = RD(s->procq);
	WR(s->devq)->next = 0;

	if(qi->open)
		(*qi->open)(RD(s->devq), s);

	c->flag |= COPEN;
	unlock(s);
	poperror();
	return s;
}

/*
 *  (Re)open a stream.  If this is the first open, create a stream.
 */
void
streamopen(Chan *c, Qinfo *qi)
{
	Stream *s;
	Queue *q;

	/*
	 *  if the stream already exists, just up the reference count.
	 */
	for(s = slist; s < &slist[conf.nstream]; s++) {
		if(s->inuse && s->type == c->type && s->dev == c->dev
		   && s->id == STREAMID(c->qid)){
			lock(s);
			if(s->inuse && s->type == c->type
			&& s->dev == c->dev
		 	&& s->id == STREAMID(c->qid)){
				s->inuse++;
				c->stream = s;
				unlock(s);
				return;
			}
			unlock(s);
		}
	}

	/*
	 *  create a new stream
	 */
	streamnew(c, qi);
}

/*
 *  On the last close of a stream, for each queue on the
 *  stream release its blocks and call its close routine.
 */
void
streamclose(Chan *c)
{
	Queue *q, *nq;
	Block *bp;
	Stream *s = c->stream;

	/*
	 *  if not open, ignore it
	 */
	if(!(c->flag & COPEN))
		return;

	/*
	 *  decrement the reference cound
	 */
	lock(s);
	if(s->inuse != 1){
		s->inuse--;
		unlock(c->stream);
		return;
	}

	/*
	 *  descend the stream closing the queues
	 */
	for(q = s->procq; q; q = q->next){
		if(q->info->close)
			(*q->info->close)(q->other);
		if(q == s->devq->other)
			break;
	}
	/*
	 *  ascend the stream freeing the queues
	 */
	for(q = s->devq; q; q = nq){
		nq = q->next;
		freeq(q);
	}
	s->id = s->dev = s->type = 0;
	s->inuse--;
	unlock(s);
}

/*
 *  put a block to be read into the queue.  wakeup any waiting reader
 */
void
stputq(Queue *q, Block *bp)
{
	int i;

	if(bp->type == M_HANGUP){
		freeb(bp);
		q->flag |= QHUNGUP;
		q->other->flag |= QHUNGUP;
	} else {
		lock(q);
		if(q->first)
			q->last->next = bp;
		else
			q->first = bp;
		q->last = bp;
		q->len += bp->wptr - bp->rptr;
		if(q->len >= Streamhi)
			q->flag |= QHIWAT;
		unlock(q);
	}
	wakeup(&q->r);
}

/*
 *  read a string.  update the offset accordingly.
 */
long
stringread(Chan *c, uchar *buf, long n, char *str)
{
	long i;

	i = strlen(str);
	i -= c->offset;
	if(i<n)
		n = i;
	if(n<0)
		return 0;
	memcpy(buf + c->offset, str, n);
	c->offset += n;
	return n;
}

/*
 *  return true if there is an output buffer available
 */
static int
isinput(void *x)
{
	return ((Queue *)x)->first != 0;
}

/*
 *  read until we fill the buffer or until a DELIM is encountered
 */
long
streamread(Chan *c, void *vbuf, long n)
{
	Block *bp;
	Stream *s;
	Queue *q;
	long rv = 0;
	int left, i, x;
	uchar *buf = vbuf;
	char num[32];

	s = c->stream;
	switch(STREAMTYPE(c->qid)){
	case Sdataqid:
		break;
	case Sctlqid:
		sprint(num, "%d", s->id);
		return stringread(c, buf, n, num);
	default:
		if(CHDIR & c->qid)
			return devdirread(c, vbuf, n, 0, 0, streamgen);
		else
			panic("streamread");
	}

	/*
	 *  one reader at a time
	 */
	qlock(&s->rdlock);
	if(waserror()){
		qunlock(&s->rdlock);
		nexterror();
	}

	/*
	 *  sleep till data is available
	 */
	q = RD(s->procq);
	left = n;
	while(left){
		bp = getq(q);
		if(bp == 0){
			if(q->flag & QHUNGUP)
				break;
			sleep(&q->r, &isinput, (void *)q);
			continue;
		}

		i = bp->wptr - bp->rptr;
		if(i <= left){
			memcpy(buf, bp->rptr, i);
			left -= i;
			buf += i;
			if(bp->flags & S_DELIM){
				freeb(bp);
				break;
			} else
				freeb(bp);
		} else {
			memcpy(buf, bp->rptr, left);
			bp->rptr += left;
			putbq(q, bp);
			left = 0;
		}
	};

	qunlock(&s->rdlock);
	poperror();
	return n - left;	
}

/*
 *  Handle a ctl request.  Streamwide requests are:
 *
 *	hangup			-- send an M_HANGUP up the stream
 *	push ldname		-- push the line discipline named ldname
 *	pop			-- pop a line discipline
 *
 *  This routing is entrered with s->wrlock'ed and must unlock.
 */
static long
streamctlwrite(Stream *s, void *a, long n)
{
	Qinfo *qi;
	Block *bp;

	/*
	 *  package
	 */
	bp = allocb(n+1);
	memcpy(bp->wptr, a, n);
	bp->wptr[n] = 0;
	bp->wptr += n + 1;

	/*
	 *  check for standard requests
	 */
	if(streamparse("hangup", bp)){
		hangup(s);
		freeb(bp);
	} else if(streamparse("push", bp)){
		qi = qinfofind((char *)bp->rptr);
		pushq(s, qi);
		freeb(bp);
	} else if(streamparse("pop", bp)){
		popq(s);
		freeb(bp);
	} else {
		bp->type = M_CTL;
		bp->flags |= S_DELIM;
		PUTNEXT(s->procq, bp);
	}

	return n;
}

/*
 *  wait till there's room in the next stream
 */
static int
notfull(void *arg)
{
	Queue *q;

	q = (Queue *)arg;
	return q->len < Streamhi;
}
void
flowctl(Queue *q)
{
	if(q->next->len >= Streamhi)
		sleep(&q->r, notfull, q->next);
}

/*
 *  send the request as a single delimited block
 */
long
streamwrite(Chan *c, void *a, long n)
{
	Stream *s;
	Block *bp;
	Queue *q;
	long rem;
	int i;

	/*
	 *  one writer at a time
	 */
	s = c->stream;
	qlock(&s->wrlock);
	if(waserror()){
		qunlock(&s->wrlock);
		nexterror();
	}

	/*
	 *  decode the qid
	 */
	switch(STREAMTYPE(c->qid)){
	case Sdataqid:
		break;
	case Sctlqid:
		n = streamctlwrite(s, a, n);
		qunlock(&s->wrlock);
		poperror();
		return n;
	default:
		panic("bad stream qid\n");
	}

	/*
	 *  No writes allowed on hungup channels
	 */
	q = s->procq;
	if(q->other->flag & QHUNGUP)
		error(0, Ehungup);

	if(GLOBAL(a) || n==0){
		/*
		 *  `a' is global to the whole system, just create a
		 *  pointer to it and pass it on.
		 */
		flowctl(q);
		bp = allocb(0);
		bp->rptr = bp->base = (uchar *)a;
		bp->wptr = bp->lim = (uchar *)a+n;
		bp->flags |= S_DELIM;
		bp->type = M_DATA;
		PUTNEXT(q, bp);
	} else {
		/*
		 *  `a' is in the user's address space, copy it into
		 *  system buffers and pass the buffers on.
		 */
		for(rem = n; ; rem -= i) {
			flowctl(q);
			bp = allocb(rem);
			i = bp->lim - bp->wptr;
			if(i >= rem){
				memcpy(bp->wptr, a, rem);
				bp->flags |= S_DELIM;
				bp->wptr += rem;
				bp->type = M_DATA;
				PUTNEXT(q, bp);
				break;
			} else {
				memcpy(bp->wptr, a, i);
				bp->wptr += i;
				bp->type = M_DATA;
				PUTNEXT(q, bp);
				a = ((char*)a) + i;
			}
		}
	}
	qunlock(&s->wrlock);
	poperror();
	return n;
}
