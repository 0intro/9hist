#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"
#include	"fcall.h"

static void pipeiput(Queue*, Block*);
static void pipeoput(Queue*, Block*);
static void pipestclose(Queue *);
Qinfo pipeinfo = { pipeiput, pipeoput, 0, pipestclose, "pipe" };

void
pipeinit(void)
{
}

void
pipereset(void)
{
}

/*
 *  allocate both streams
 *
 *  a subsequent clone will get them the second stream
 */
Chan*
pipeattach(char *spec)
{
	Chan *c;
	int i;

	/*
	 *  make the first stream
	 */
	c = devattach('|', spec);
	c->qid = STREAMQID(0, Sdataqid);
	streamnew(c, &pipeinfo);
	return c;
}

Chan*
pipeclone(Chan *c, Chan *nc)
{
	/*
	 *  make the second stream 
	 */
	nc = devclone(c, nc);
	if(waserror()){
		close(nc);
		nexterror();
	}
	nc->qid = STREAMQID(1, Sdataqid);
	streamnew(nc, &pipeinfo);
	poperror();

	/*
	 *  attach it to the first
	 */
	c->stream->devq->ptr = (Stream *)nc->stream;
	nc->stream->devq->ptr = (Stream *)c->stream;
	c->stream->devq->other->next = nc->stream->devq;
	nc->stream->devq->other->next = c->stream->devq;

	/*
	 *  up the inuse count of each stream to reflect the
	 *  pointer from the other stream.
	 */
	if(streamenter(c->stream)<0)
		panic("pipeattach");
	if(streamenter(nc->stream)<0)
		panic("pipeattach");
	return nc;
}

int
pipewalk(Chan *c, char *name)
{
	print("pipewalk\n");
	error(0, Egreg);
}

void
pipestat(Chan *c, char *db)
{
	streamstat(c, db, "pipe");
}

Chan *
pipeopen(Chan *c, int omode)
{
	c->mode = omode;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
pipecreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Egreg);
}

void
piperemove(Chan *c)
{
	error(0, Egreg);
}

void
pipewstat(Chan *c, char *db)
{
	error(0, Eperm);
}

void
pipeclose(Chan *c)
{
	Stream *other;

	other = (Stream *)c->stream->devq->ptr;

	if(waserror()){
		streamexit(other, 0);
		nexterror();
	}
	streamclose(c);		/* close this stream */
	streamexit(other, 0);	/* release stream for other half of pipe */
	poperror();
}

long
piperead(Chan *c, void *va, long n)
{
	return streamread(c, va, n);
}

long
pipewrite(Chan *c, void *va, long n)
{
	if(waserror()){
		postnote(u->p, 1, "sys: write on closed pipe", NExit);
		error(0, Egreg);
	}
	n = streamwrite(c, va, n, 0);
	poperror();
	return n;
}

void
pipeuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void
pipeerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

/*
 *  stream stuff
 */
/*
 *  send a block up stream to the process.
 *  sleep untill there's room upstream.
 */
static void
pipeiput(Queue *q, Block *bp)
{
	FLOWCTL(q);
	PUTNEXT(q, bp);
}

/*
 *  send the block to the other side without letting the connection
 *  disappear in mid put.
 */
static void
pipeoput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

/*
 *  send a hangup and disconnect the streams
 */
static void
pipestclose(Queue *q)
{
	Block *bp;

	/*
	 *  point to the bit-bucket and let any in-progress
	 *  write's finish.
	 */
	q->put = nullput;
	wakeup(&q->r);

	/*
	 *  send a hangup
	 */
	q = q->other;
	bp = allocb(0);
	bp->type = M_HANGUP;
	PUTNEXT(q, bp);
}
