#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"
#include	"fcall.h"

typedef struct Pipe	Pipe;

struct Pipe
{
	Ref;
	int	debug;
	Pipe	*next;
};

struct Pipealloc
{
	Lock;
	Pipe *pipe;
	Pipe *free;
} pipealloc;

static void pipeiput(Queue*, Block*);
static void pipeoput(Queue*, Block*);
static void pipestclose(Queue *);
Qinfo pipeinfo = { pipeiput, pipeoput, 0, pipestclose, "pipe" };

Dirtab pipedir[]={
	"data",		Sdataqid,	0,			0600,
	"ctl",		Sctlqid,	0,			0600,
	"data1",	Sdataqid,	0,			0600,
	"ctl1",		Sctlqid,	0,			0600,
};
#define NPIPEDIR 4

void
pipeinit(void)
{
}

/*
 *  allocate structures for conf.npipe pipes
 */
void
pipereset(void)
{
	Pipe *p, *ep;

	pipealloc.pipe = ialloc(conf.npipe * sizeof(Pipe), 0);
	ep = &pipealloc.pipe[conf.npipe-1];
	for(p = pipealloc.pipe; p < ep; p++)
		p->next = p+1;
	pipealloc.free = pipealloc.pipe;
}

/*
 *  create a pipe, no streams are created until an open
 */
Chan*
pipeattach(char *spec)
{
	Pipe *p;
	Chan *c;

	c = devattach('|', spec);

	lock(&pipealloc);
	if(pipealloc.free == 0){
		unlock(&pipealloc);
		error(0, Enopipe);
	}
	p = pipealloc.free;
	pipealloc.free = p->next;
	p->ref = 1;
	unlock(&pipealloc);

	c->qid = CHDIR|STREAMQID(2*(p - pipealloc.pipe), 0);
	return c;
}

Chan*
pipeclone(Chan *c, Chan *nc)
{
	Pipe *p;

	p = &pipealloc.pipe[STREAMID(c->qid)/2];
	nc = devclone(c, nc);
	incref(p);
	return nc;
}

int
pipegen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	int id;

	id = STREAMID(c->qid);
	if(i > 1)
		id++;
	if(tab==0 || i>=ntab)
		return -1;
	tab += i;
	devdir(c, STREAMQID(id, tab->qid), tab->name, tab->length, tab->perm, dp);
	return 1;
}


int
pipewalk(Chan *c, char *name)
{
	return devwalk(c, name, pipedir, NPIPEDIR, pipegen);
}

void
pipestat(Chan *c, char *db)
{
	streamstat(c, db, "pipe");
}

/*
 *  if the stream doesn't exist, create it
 */
Chan *
pipeopen(Chan *c, int omode)
{
	Pipe *p;
	Stream *local, *remote;

	if(CHDIR & c->qid){
		if(omode != OREAD)
			error(0, Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	p = &pipealloc.pipe[STREAMID(c->qid)/2];
	remote = 0;
	if(waserror()){
		unlock(p);
		if(remote)
			streamclose1(remote);
		nexterror();
	}
	lock(p);
	streamopen(c, &pipeinfo);
	local = c->stream;
	if(local->devq->ptr == 0){
		/*
		 *  First stream opened, create the other end also
		 */
		remote = streamnew(c->type, c->dev, STREAMID(c->qid)^1, &pipeinfo, 1);

		/*
		 *  connect the device ends of both streams
		 */
		local->devq->ptr = remote;
		remote->devq->ptr = local;
		local->devq->other->next = remote->devq;
		remote->devq->other->next = local->devq;

		/*
		 *  increment the inuse count to reflect the
		 *  pointer from the other stream.
		 */
		if(streamenter(local)<0)
			panic("pipeattach");
	}
	unlock(p);
	poperror();

	c->mode = omode&~OTRUNC;
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
pipeexit(Pipe *p)
{
	decref(p);
	if(p->ref <= 0){
		lock(&pipealloc);
		p->next = pipealloc.free;
		pipealloc.free = p;
		unlock(&pipealloc);
	}
}

void
pipeclose(Chan *c)
{
	Stream *remote;
	Stream *local;
	Pipe *p;

	p = &pipealloc.pipe[STREAMID(c->qid)/2];

	/*
	 *  take care of assosiated streams
	 */
	if(local = c->stream){
		remote = (Stream *)c->stream->devq->ptr;
		if(waserror()){
			streamexit(remote, 0);
			pipeexit(p);
			nexterror();
		}
		streamclose(c);		/* close this stream */
		streamexit(remote, 0);	/* release stream for other half of pipe */
		poperror();
	}
	pipeexit(p);
}

long
piperead(Chan *c, void *va, long n)
{
	if(CHDIR&c->qid)
		return devdirread(c, va, n, pipedir, NPIPEDIR, pipegen);
	else
		return streamread(c, va, n);
}

/*
 *  a write to a closed pipe causes a note to be sent to
 *  the process.
 */
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
