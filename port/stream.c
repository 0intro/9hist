#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"devtab.h"
#include	"fcall.h"

enum {
	Nclass=4,	/* number of block classes */
};

/*
 *  process end line discipline
 */
static void stputq(Queue*, Block*);
Qinfo procinfo =
{
	stputq,
	nullput,
	0,
	0,
	"process"
};

/*
 *  line disciplines that can be pushed
 */
static Qinfo *lds;

/*
 *  All stream structures are ialloc'd at boot time
 */
Stream *slist;
Queue *qlist;
static Lock garbagelock;

/*
 *  The block classes.  There are Nclass block sizes, each with its own free list.
 *  All are ialloced at qinit() time.
 */
typedef struct {
	int	size;
	int	lim;
	int	made;
	Blist;
	QLock;		/* qlock for sleepers on r */
	Rendez	r;	/* sleep here waiting for blocks */
} Bclass;
Bclass bclass[Nclass]={
	{ 0 },
	{ 68 },
	{ 260 },
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
	Bclass *bcp;

	/*
	 *  allocate queues, streams
	 */
	slist = (Stream *)ialloc(conf.nstream * sizeof(Stream), 0);
	qlist = (Queue *)ialloc(conf.nqueue * sizeof(Queue), 0);

	/*
	 *  set limits on blocks
	 */
	n = conf.nblock;
	for(class = 0; class < Nclass; class++){
		if(class < Nclass-1)
			n = n/2;
		bcp = &bclass[class];
		bcp->lim = n;
		bcp->made = 0;
	}

	/*
	 *  make stream modules available
	 */
	streaminit0();
}

/*
 *  make known a stream module and call its initialization routine, if
 *  it has one.
 */
void
newqinfo(Qinfo *qi)
{
	qi->next = lds;
	lds = qi;
	if(qi->reset)
		(*qi->reset)();
}

/*
 *  upgrade a block 0 block to another class (called with bcp qlocked)
 */
int
newblock(Bclass *bcp)
{
	Page *page;
	int n;
	Block *bp;
	uchar *cp;

	if(bcp->made > bcp->lim)
		return -1;

	if(bcp == bclass){
		/*
		 *  create some level zero blocks and return
		 */
		page = newpage(1, 0, 0);
		page->va = VA(kmap(page));
		n = BY2PG/sizeof(Block);
		bp = (Block *)(page->va);
		while(n-- > 0){
			bp->flags = 0;
			bp->base = bp->lim = bp->rptr = bp->wptr = 0;
			if(bcp->first)
				bcp->last->next = bp;
			else
				bcp->first = bp;
			bcp->last = bp;
			bcp->made++;
			bp++;
		}
	} else {
		/*
		 *  create a page worth of new blocks
		 */
		page = newpage(1, 0, 0);
		page->va = VA(kmap(page));
		n = BY2PG/bcp->size;
		cp = (uchar *)(page->va);
		
		while(n-- > 0){
			/*
			 *  upgrade a level 0 block
			 */
			bp = allocb(0);
			qlock(bclass);
			bclass->made--;
			bcp->made++;
			bp->flags = bcp - bclass;
			qunlock(bclass);

			/*
			 *  tack on the data area
			 */
			bp->base = bp->rptr = bp->wptr = cp;
			cp += bcp->size;
			bp->lim = cp;
			if(bcp->first)
				bcp->last->next = bp;
			else
				bcp->first = bp;
			bcp->last = bp;
		}
	}
	return 0;
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

	/*
	 *  map size to class
	 */
	for(bcp=bclass; bcp->size<size && bcp<&bclass[Nclass-1]; bcp++)
		;

	/*
	 *  look for a free block
	 */
	lock(bcp);
	while(bcp->first == 0){
		if(newblock(bcp) == 0)
			continue;
		unlock(bcp);
		qlock(bcp);
		if(waserror()){
			qunlock(bcp);
			nexterror();
		}
		tsleep(&bcp->r, isblock, (void *)bcp, 250);
		qunlock(bcp);
		poperror();
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
 *  Free a block (or list of blocks).  Poison its pointers so that
 *  someone trying to access it after freeing will cause a dump.
 */
void
freeb(Block *bp)
{
	Bclass *bcp;
	int tries;
	int x;

	if((bp->flags&S_CLASS) >= Nclass)
		panic("freeb class");
	bcp = &bclass[bp->flags & S_CLASS];
	lock(bcp);
	bp->rptr = bp->wptr = 0;
	if(bcp->first)
		bcp->last->next = bp;
	else
		bcp->first = bp;
	tries = 0;
	while(bp->next){
		if(++tries > 10){
			dumpstack();
			panic("freeb");
		}
		bp = bp->next;
	}
	bcp->last = bp;
	unlock(bcp);
	if(bcp->r.p)
		wakeup(&bcp->r);
}

/*
 *  pad a block to the front with n bytes
 */
Block *
padb(Block *bp, int n)
{
	Block *nbp;

	if(bp->base && bp->rptr-bp->base>=n){
		bp->rptr -= n;
		return bp;
	} else {
		nbp = allocb(n);
		nbp->wptr = nbp->lim;
		nbp->rptr = nbp->wptr - n;
		nbp->next = bp;
		return nbp;
	}
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
		error(Enoqueue);
	}

	q->flag = QINUSE;
	q->r.p = 0;
	q->info = qi;
	q->put = qi->iput;
	q->len = q->nb = 0;
	q->ptr = 0;
	q->rp = &q->r;
	wq = q->other = q + 1;

	wq->flag = QINUSE;
	wq->r.p = 0;
	wq->info = qi;
	wq->put = qi->oput;
	wq->other = q;
	wq->ptr = 0;
	wq->len = wq->nb = 0;
	wq->rp = &wq->r;

	unlock(q);

	return q;
}

/*
 *  flush a queue
 */
static void
flushq(Queue *q)
{
	Block *bp;

	q = RD(q);
	while(bp = getq(q))
		freeb(bp);
	q = WR(q);
	while(bp = getq(q))
		freeb(bp);
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
		error(Ebadld);
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

	lock(q);
	if(q->first)
		q->last->next = bp;
	else
		q->first = bp;
	q->len += BLEN(bp);
	q->nb++;
	delim = bp->flags & S_DELIM;
	while(bp->next) {
		bp = bp->next;
		q->len += BLEN(bp);
		q->nb++;
		delim |= bp->flags & S_DELIM;
	}
	q->last = bp;
	if(q->len >= Streamhi || q->nb >= Streambhi)
		q->flag |= QHIWAT;
	unlock(q);
	return delim;
}

int
blen(Block *bp)
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
 * bround - round a block chain to some 2^n number of bytes
 */
int
bround(Block *bp, int amount)
{
	Block *last;
	int len, pad;

	len = 0;
	SET(last);	/* Ken's magic */

	while(bp) {
		len += BLEN(bp);
		last = bp;
		bp = bp->next;
	}

	pad = ((len + amount) & ~amount) - len;
	if(pad) {
		last->next = allocb(pad);
		last->flags &= ~S_DELIM;
		last = last->next;
		memset(last->wptr, 0, pad);
		last->wptr += pad;
		last->flags |= S_DELIM;
		
	}

	return len + pad;
}

int
putb(Blist *q, Block *bp)
{
	int delim;

	if(q->first)
		q->last->next = bp;
	else
		q->first = bp;
	q->len += BLEN(bp);
	delim = bp->flags & S_DELIM;
	while(bp->next) {
		bp = bp->next;
		q->len += BLEN(bp);
		delim |= bp->flags & S_DELIM;
	}
	q->last = bp;
	return delim;
}

/*
 *  add a block to the start of a queue 
 */
void
putbq(Blist *q, Block *bp)
{
	lock(q);
	if(q->first)
		bp->next = q->first;
	else
		q->last = bp;
	q->first = bp;
	q->len += BLEN(bp);
	q->nb++;
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
		q->len -= BLEN(bp);
		q->nb--;
		if((q->flag&QHIWAT) && q->len<Streamhi/2 && q->nb<Streambhi/2){
			wakeup(q->other->next->other->rp);
			q->flag &= ~QHIWAT;
		}
		bp->next = 0;
	}
	unlock(q);
	return bp;
}

/*
 *  remove the first block from a list of blocks
 */
Block *
getb(Blist *q)
{
	Block *bp;

	bp = q->first;
	if(bp) {
		q->first = bp->next;
		if(q->first == 0)
			q->last = 0;
		q->len -= BLEN(bp);
		bp->next = 0;
	}
	return bp;
}

/*
 *  make sure the first block has n bytes
 */
Block *
pullup(Block *bp, int n)
{
	Block *nbp;
	int i;

	/*
	 *  this should almost always be true, the rest it
	 *  just for to avoid every caller checking.
	 */
	if(BLEN(bp) >= n)
		return bp;

	/*
	 *  if not enough room in the first block,
	 *  add another to the front of the list.
	if(bp->lim - bp->rptr < n){
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
			memmove(bp->wptr, nbp->rptr, n);
			bp->wptr += n;
			nbp->rptr += n;
			return bp;
		} else {
			memmove(bp->wptr, nbp->rptr, i);
			bp->wptr += i;
			bp->next = nbp->next;
			nbp->next = 0;
			freeb(nbp);
		}
	}
	freeb(bp);
	return 0;
}

/*
 *  grow the front of a list of blocks by n bytes
 */
Block *
prepend(Block *bp, int n)
{
	Block *nbp;

	if(bp->base && (bp->rptr - bp->base)>=n){
		/*
		 *  room for channel number in first block of message
		 */
		bp->rptr -= n;
		return bp;
	} else {
		/*
		 *  make new block, put message number at end
		 */
		nbp = allocb(2);
		nbp->next = bp;
		nbp->wptr = nbp->lim;
		nbp->rptr = nbp->wptr - n;
		return nbp;
	}
}


/*
 *  put a block into the bit bucket
 */
void
nullput(Queue *q, Block *bp)
{
	if(bp->type == M_HANGUP)
		freeb(bp);
	else {
		freeb(bp);
		error(Ehungup);
	}
}

/*
 *  find the info structure for line discipline 'name'
 */
static Qinfo *
qinfofind(char *name)
{
	Qinfo *qi;

	if(name == 0)
		error(Ebadld);
	for(qi = lds; qi; qi = qi->next)
		if(strcmp(qi->name, name)==0)
			return qi;
	error(Ebadld);
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
	if(BLEN(bp) < len)
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
	"data",		{Sdataqid},	0,			0600,
	"ctl",		{Sctlqid},	0,			0600,
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

	devdir(c, (Qid){STREAMQID(STREAMID(c->qid.path),tab->qid.path), 0}, tab->name, tab->length,
		tab->perm, dp);
	return 1;
}

/*
 *  create a new stream, if noopen is non-zero, don't increment the open count
 */
Stream *
streamnew(ushort type, ushort dev, ushort id, Qinfo *qi, int noopen)
{
	Stream *s;
	Queue *q;

	/*
	 *  find a free stream struct
	 */
	for(s = slist; s < &slist[conf.nstream]; s++) {
		if(s->inuse == 0){
			if(canqlock(s)){
				if(s->inuse == 0)
					break;
				qunlock(s);
			}
		}
	}
	if(s == &slist[conf.nstream]){
		print("no more streams\n");
		error(Enostream);
	}
	if(waserror()){
		qunlock(s);
		streamclose1(s);
		nexterror();
	}

	/*
	 *  identify the stream
	 */
	s->type = type;
	s->dev = dev;
	s->id = id;

	/*
 	 *  hang a device and process q off the stream
	 */
	s->inuse = 1;
	if(noopen)
		s->opens = 0;
	else
		s->opens = 1;
	s->hread = 0;
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

	qunlock(s);
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
	 *  if the stream already exists, just increment the reference counts.
	 */
	for(s = slist; s < &slist[conf.nstream]; s++) {
		if(s->inuse && s->type == c->type && s->dev == c->dev
		   && s->id == STREAMID(c->qid.path)){
			qlock(s);
			if(s->inuse && s->type == c->type
			&& s->dev == c->dev
		 	&& s->id == STREAMID(c->qid.path)){
				s->inuse++;
				s->opens++;
				c->stream = s;
				qunlock(s);
				return;
			}
			qunlock(s);
		}
	}

	/*
	 *  create a new stream
	 */
	c->stream = streamnew(c->type, c->dev, STREAMID(c->qid.path), qi, 0);
}

/*
 *  Enter a stream.  Increment the reference count so it can't disappear
 *  under foot.
 */
int
streamenter(Stream *s)
{
	qlock(s);
	if(s->opens == 0){
		qunlock(s);
		return -1;
	}
	s->inuse++;
	qunlock(s);
	return 0;
}

/*
 *  Decrement the reference count on a stream.  If the count is
 *  zero, free the stream.
 */
int
streamexit(Stream *s, int locked)
{
	Queue *q;
	Queue *nq;
	int rv;
	char *name;

	if(!locked)
		qlock(s);
	if(s->inuse == 1){
		if(s->opens != 0)
			panic("streamexit %d %s\n", s->opens, s->devq->info->name);

		/*
		 *  ascend the stream freeing the queues
		 */
		for(q = s->devq; q; q = nq){
			nq = q->next;
			freeq(q);
		}
		s->id = s->dev = s->type = 0;
	}
	s->inuse--;
	rv = s->inuse;
	if(!locked)
		qunlock(s);
	return rv;
}

/*
 *  On the last close of a stream, for each queue on the
 *  stream release its blocks and call its close routine.
 */
int
streamclose1(Stream *s)
{
	Queue *q, *nq;
	Block *bp;
	int rv;

	/*
	 *  decrement the reference count
	 */
	qlock(s);
	if(s->opens == 1){
		/*
		 *  descend the stream closing the queues
		 */
		for(q = s->procq; q; q = q->next){
			if(!waserror()){
				if(q->info->close)
					(*q->info->close)(q->other);
				poperror();
			}
			WR(q)->put = nullput;

			/*
			 *  this may be 2 streams joined device end to device end
			 */
			if(q == s->devq->other)
				break;
		}
	
		/*
		 *  ascend the stream flushing the queues
		 */
		for(q = s->devq; q; q = nq){
			nq = q->next;
			flushq(q);
		}
	}
	rv = --(s->opens);

	/*
	 *  leave it and free it
	 */
	streamexit(s, 1);
	qunlock(s);
	return rv;
}
int
streamclose(Chan *c)
{
	/*
	 *  if no stream, ignore it
	 */
	if(!c->stream)
		return 0;
	return streamclose1(c->stream);
}

/*
 *  put a block to be read into the queue.  wakeup any waiting reader
 */
void
stputq(Queue *q, Block *bp)
{
	int delim;

	if(bp->type == M_HANGUP){
		freeb(bp);
		q->flag |= QHUNGUP;
		q->other->flag |= QHUNGUP;
		wakeup(q->other->rp);
		delim = 1;
	} else {
		lock(q);
		if(q->first)
			q->last->next = bp;
		else
			q->first = bp;
		q->len += BLEN(bp);
		q->nb++;
		delim = bp->flags & S_DELIM;
		while(bp->next) {
			bp = bp->next;
			q->len += BLEN(bp);
			q->nb++;
			delim |= bp->flags & S_DELIM;
		}
		q->last = bp;
		if(q->len >= Streamhi || q->nb >= Streambhi){
			q->flag |= QHIWAT;
			delim = 1;
		}
		unlock(q);
	}
	if(delim)
		wakeup(q->rp);
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
	memmove(buf, str + c->offset, n);
	return n;
}

/*
 *  return the stream id
 */
long
streamctlread(Chan *c, void *vbuf, long n)
{
	uchar *buf = vbuf;
	char num[32];
	Stream *s;

	s = c->stream;
	if(STREAMTYPE(c->qid.path) == Sctlqid){
		sprint(num, "%d", s->id);
		return stringread(c, buf, n, num);
	} else {
		if(CHDIR & c->qid.path)
			return devdirread(c, vbuf, n, 0, 0, streamgen);
		else
			panic("streamctlread");
	}
}

/*
 *  return true if there is an output buffer available
 */
static int
isinput(void *x)
{
	Queue *q;

	q = (Queue *)x;
	return (q->flag&QHUNGUP) || q->first!=0;
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
	int left, i;
	uchar *buf = vbuf;

	if(STREAMTYPE(c->qid.path) != Sdataqid)
		return streamctlread(c, vbuf, n);

	/*
	 *  one reader at a time
	 */
	s = c->stream;
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
			if(q->flag & QHUNGUP){
				if(s->hread++ < 3)
					break;
				else
					error(Ehungup);
			}
			q->rp = &q->r;
			sleep(q->rp, &isinput, (void *)q);
			continue;
		}

		i = BLEN(bp);
		if(i <= left){
			memmove(buf, bp->rptr, i);
			left -= i;
			buf += i;
			if(bp->flags & S_DELIM){
				freeb(bp);
				break;
			} else
				freeb(bp);
		} else {
			memmove(buf, bp->rptr, left);
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
 *  look for an instance of the line discipline `name' on
 *  the stream `s'
 */
void
qlook(Stream *s, char *name)
{
	Queue *q;

	for(q = s->procq; q; q = q->next){
		if(strcmp(q->info->name, name) == 0)
			return;

		/*
		 *  this may be 2 streams joined device end to device end
		 */
		if(q == s->devq->other)
			break;
	}
	errors("not found");
}

/*
 *  Handle a ctl request.  Streamwide requests are:
 *
 *	hangup			-- send an M_HANGUP up the stream
 *	push ldname		-- push the line discipline named ldname
 *	pop			-- pop a line discipline
 *	look ldname		-- look for a line discipline
 *
 *  This routing is entrered with s->wrlock'ed and must unlock.
 */
static long
streamctlwrite(Chan *c, void *a, long n)
{
	Qinfo *qi;
	Block *bp;
	Stream *s;

	if(STREAMTYPE(c->qid.path) != Sctlqid)
		panic("streamctlwrite %lux", c->qid);
	s = c->stream;

	/*
	 *  package
	 */
	bp = allocb(n+1);
	memmove(bp->wptr, a, n);
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
	} else if(streamparse("look", bp)){
		qlook(s, (char *)bp->rptr);
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
	return !QFULL((Queue *)arg);
}
void
flowctl(Queue *q)
{
	qlock(&q->rlock);
	if(waserror()){
		qunlock(&q->rlock);
		nexterror();
	}
	q->rp = &q->r;
	sleep(q->rp, notfull, q->next);
	qunlock(&q->rlock);
	poperror();
}

/*
 *  send the request as a single delimited block
 */
long
streamwrite(Chan *c, void *a, long n, int docopy)
{
	Stream *s;
	Block *bp;
	Queue *q;
	long rem;
	int i;

	s = c->stream;

	/*
	 *  decode the qid
	 */
	if(STREAMTYPE(c->qid.path) != Sdataqid)
		return streamctlwrite(c, a, n);

	/*
	 *  No writes allowed on hungup channels
	 */
	q = s->procq;
	if(q->other->flag & QHUNGUP)
		error(Ehungup);

	if(!docopy && GLOBAL(a)){
		/*
		 *  `a' is global to the whole system, just create a
		 *  pointer to it and pass it on.
		 */
		FLOWCTL(q);
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
			FLOWCTL(q);
			bp = allocb(rem);
			i = bp->lim - bp->wptr;
			if(i >= rem){
				memmove(bp->wptr, a, rem);
				bp->flags |= S_DELIM;
				bp->wptr += rem;
				bp->type = M_DATA;
				PUTNEXT(q, bp);
				break;
			} else {
				memmove(bp->wptr, a, i);
				bp->wptr += i;
				bp->type = M_DATA;
				PUTNEXT(q, bp);
				a = ((char*)a) + i;
			}
		}
	}
	return n;
}

/*
 *  like andrew's getmfields but no hidden state
 */
int
getfields(char *lp,	/* to be parsed */
	char **fields,	/* where to put pointers */
	int n,		/* number of pointers */
	char sep	/* separator */
)
{
	int i;

	for(i=0; lp && *lp && i<n; i++){
		while(*lp == sep)
			*lp++=0;
		if(*lp == 0)
			break;
		fields[i]=lp;
		while(*lp && *lp != sep)
			lp++;
	}
	return i;
}

/*
 *  stat a stream.  the length is the number of bytes up to the
 *  first delimiter.
 */
void
streamstat(Chan *c, char *db, char *name)
{
	Dir dir;
	Stream *s;
	Queue *q;
	Block *bp;
	long n;

	s = c->stream;
	if(s == 0)
		n = 0;
	else {
		q = RD(s->procq);
		lock(q);
		for(n=0, bp=q->first; bp; bp = bp->next){
			n += BLEN(bp);
			if(bp->flags&S_DELIM)
				break;
		}
		unlock(q);
	}

	devdir(c, c->qid, name, n, 0, &dir);
	convD2M(&dir, db);
}

/*
 *  Dump all block information of how many blocks are in which queues
 */
void
dumpblocks(Queue *q, char c)
{
	Block *bp;
	uchar *cp;

	lock(q);
	for(bp = q->first; bp; bp = bp->next){
		print("%c%d%c", c, bp->wptr-bp->rptr, (bp->flags&S_DELIM)?'D':' ');
		for(cp = bp->rptr; cp<bp->wptr && cp<bp->rptr+10; cp++)
			print(" %uo", *cp);
		print("\n");
	}
	unlock(q);
}

void
dumpqueues(void)
{
	Queue *q;
	int count, qcount;
	Block *bp;
	Bclass *bcp;

	print("\n");
	qcount = 0;
	for(q = qlist; q < qlist + conf.nqueue; q++, q++){
		if(!(q->flag & QINUSE))
			continue;
		qcount++;
		print("%10s %ux  R n %d l %d f %ux r %ux", q->info->name, q,
			q->nb, q->len, q->flag, &(q->r));
		print("  W n %d l %d f %ux r %ux next %lux put %lux Rz %lux\n", 
			WR(q)->nb, WR(q)->len,
			WR(q)->flag, &(WR(q)->r), q->next, q->put, q->rp);
		dumpblocks(q, 'R');
		dumpblocks(WR(q), 'W');
	}
	print("%d queues\n", qcount);
	for(bcp=bclass; bcp<&bclass[Nclass]; bcp++){
		lock(bcp);
		for(count = 0, bp = bcp->first; bp; count++, bp = bp->next)
			;
		unlock(bcp);
		print("%d blocks of size %d\n", count, bcp->size);
	}
	print("\n");
}
