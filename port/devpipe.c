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
Qinfo pipeinfo =
{
	pipeiput,
	pipeoput,
	0,
	pipestclose,
	"pipe"
};

Dirtab pipedir[]={
	"data",		{Sdataqid},	0,			0600,
	"ctl",		{Sctlqid},	0,			0600,
	"data1",	{Sdataqid},	0,			0600,
	"ctl1",		{Sctlqid},	0,			0600,
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
		close(c);
		error(Enopipe);
	}
	p = pipealloc.free;
	pipealloc.free = p->next;
	if(incref(p) != 1)
		panic("pipeattach");
	unlock(&pipealloc);

	c->qid = (Qid){CHDIR|STREAMQID(2*(p - pipealloc.pipe), 0), 0};
	return c;
}

Chan*
pipeclone(Chan *c, Chan *nc)
{
	Pipe *p;

	p = &pipealloc.pipe[STREAMID(c->qid.path)/2];
	nc = devclone(c, nc);
	if(incref(p) <= 1)
		panic("pipeclone");
	return nc;
}

int
pipegen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	int id;

	id = STREAMID(c->qid.path);
	if(i > 1)
		id++;
	if(tab==0 || i>=ntab)
		return -1;
	tab += i;
	devdir(c, (Qid){STREAMQID(id, tab->qid.path),0}, tab->name, tab->length, tab->perm, dp);
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

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	p = &pipealloc.pipe[STREAMID(c->qid.path)/2];
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
		remote = streamnew(c->type, c->dev, STREAMID(c->qid.path)^1, &pipeinfo, 1);

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
			panic("pipeopen");
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
	error(Egreg);
}

void
piperemove(Chan *c)
{
	error(Egreg);
}

void
pipewstat(Chan *c, char *db)
{
	error(Eperm);
}

void
pipeclose(Chan *c)
{
	Pipe *p;
	Stream *remote;

	p = &pipealloc.pipe[STREAMID(c->qid.path)/2];

	/*
	 *  take care of associated streams
	 */
	if(c->stream){
		remote = c->stream->devq->ptr;
		if(streamclose(c) <= 0){
			if(remote)
				streamexit(remote, 0);
		}
	}

	/*
	 *  free the structure
	 */
	if(decref(p) == 0){
		lock(&pipealloc);
		p->next = pipealloc.free;
		pipealloc.free = p;
		unlock(&pipealloc);
	}
	if(p->ref < 0)
		panic("pipeexit");
}

long
piperead(Chan *c, void *va, long n, ulong offset)
{
	if(c->qid.path & CHDIR)
		return devdirread(c, va, n, pipedir, NPIPEDIR, pipegen);
	else
		return streamread(c, va, n);
}

/*
 *  a write to a closed pipe causes a note to be sent to
 *  the process.
 */
long
pipewrite(Chan *c, void *va, long n, ulong offset)
{
	if(waserror()){
		postnote(u->p, 1, "sys: write on closed pipe", NExit);
		error(Egreg);
	}
	n = streamwrite(c, va, n, 0);
	poperror();
	return n;
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
	if(bp->type != M_HANGUP)
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
	Stream *remote;

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
	if(q->next == 0)
		return;
	bp = allocb(0);
	bp->type = M_HANGUP;
	PUTNEXT(q, bp);
}
