#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

struct Mntrpc
{
	Chan*	c;		/* Channel for whom we are working */
	Mntrpc*	list;		/* Free/pending list */
	Fcall	request;	/* Outgoing file system protocol message */
	Fcall	reply;		/* Incoming reply */
	Mnt*	m;		/* Mount device during rpc */
	Rendez	r;		/* Place to hang out */
	char*	rpc;		/* I/O Data buffer */
	char	done;		/* Rpc completed */
	char	flushed;	/* Flush was sent */
	ushort	flushtag;	/* Tag flush sent on */
	char	flush[MAXMSG];	/* Somewhere to build flush */
	uvlong	stime;		/* start time for mnt statistics */
	ulong	reqlen;		/* request length for mnt statistics */
	ulong	replen;		/* reply length for mnt statistics */
};

struct Mntalloc
{
	Lock;
	Mnt*	list;		/* Mount devices in use */
	Mnt*	mntfree;	/* Free list */
	Mntrpc*	rpcfree;
	int	nrpcfree;
	int	nrpcused;
	ulong	id;
	int	rpctag;
}mntalloc;

#define MAXRPC		(16*1024+MAXMSG)

void	mattach(Mnt*, Chan*, char*);
void	mntauth(Mnt*, Mntrpc*, char*, ushort);
Mnt*	mntchk(Chan*);
void	mntdirfix(uchar*, Chan*);
int	mntflush(Mnt*, Mntrpc*);
void	mntfree(Mntrpc*);
void	mntgate(Mnt*);
void	mntpntfree(Mnt*);
void	mntqrm(Mnt*, Mntrpc*);
Mntrpc*	mntralloc(Chan*);
long	mntrdwr(int, Chan*, void*, long, vlong);
long	mnt9prdwr(int, Chan*, void*, long, vlong);
void	mntrpcread(Mnt*, Mntrpc*);
void	mountio(Mnt*, Mntrpc*);
void	mountmux(Mnt*, Mntrpc*);
void	mountrpc(Mnt*, Mntrpc*);
int	rpcattn(void*);
void	mclose(Mnt*, Chan*);
void	mntrecover(Mnt*, Mntrpc*);
Chan*	mntchan(void);

int defmaxmsg = MAXFDATA;
void (*mntstats)(int, Chan*, uvlong, ulong);

enum
{
	Tagspace	= 1,
	Tagfls		= 0x8000,
	Tagend		= 0xfffe,
};

static void
mntreset(void)
{
	mntalloc.id = 1;
	mntalloc.rpctag = Tagspace;

	cinit();
}

static Chan*
mntattach(char *muxattach)
{
	Mnt *m;
	Chan *c, *mc;
	char buf[NAMELEN];
	struct bogus{
		Chan	*chan;
		char	*spec;
		int	flags;
	}bogus;

	bogus = *((struct bogus *)muxattach);
	c = bogus.chan;

	lock(&mntalloc);
	for(m = mntalloc.list; m; m = m->list) {
		if(m->c == c && m->id) {
			lock(m);
			if(m->id && m->ref > 0 && m->c == c) {
				m->ref++;
				unlock(m);
				unlock(&mntalloc);
				c = mntchan();
				if(waserror()) {
					chanfree(c);
					nexterror();
				}
				mattach(m, c, bogus.spec);
				poperror();
				if(bogus.flags&MCACHE)
					c->flag |= CCACHE;
				return c;
			}
			unlock(m);
		}
	}

	m = mntalloc.mntfree;
	if(m != 0)
		mntalloc.mntfree = m->list;
	else {
		m = malloc(sizeof(Mnt));
		if(m == 0) {
			unlock(&mntalloc);
			exhausted("mount devices");
		}
		m->flushbase = Tagfls;
		m->flushtag = Tagfls;
	}
	m->list = mntalloc.list;
	mntalloc.list = m;
	m->id = mntalloc.id++;
	unlock(&mntalloc);

	lock(m);
	m->ref = 1;
	m->queue = 0;
	m->rip = 0;
	m->c = c;
	m->c->flag |= CMSG;
	if(strncmp(bogus.spec, "mntblk=", 7) == 0) {
		m->blocksize = strtoul(bogus.spec+7, 0, 0);
		if(m->blocksize > MAXFDATA)
			m->blocksize = MAXFDATA;
		print("mount blk %d\n", m->blocksize);
		bogus.spec = "";
	}
	else
		m->blocksize = defmaxmsg;
	m->flags = bogus.flags & ~MCACHE;

	incref(m->c);

	sprint(buf, "#M%lud", m->id);

	unlock(m);

	c = mntchan();
	if(waserror()) {
		mclose(m, c);
		/* Close must not be called since it will
		 * call mnt recursively
		 */
		chanfree(c);
		nexterror();
	}

	mattach(m, c, bogus.spec);
	poperror();

	/*
	 * Detect a recursive mount for a mount point served by exportfs.
	 * If CHDIR is clear in the returned qid, the foreign server is
	 * requesting the mount point be folded into the connection
	 * to the exportfs. In this case the remote mount driver does
	 * the multiplexing.
	 */
	mc = m->c;
	if(mc->type == devno('M', 0) && (c->qid.path&CHDIR) == 0) {
		mclose(m, c);
		c->qid.path |= CHDIR;
		c->mntptr = mc->mntptr;
		c->mchan = c->mntptr->c;
		c->mqid = c->qid;
		incref(c->mntptr);
	}

	if(bogus.flags & MCACHE)
		c->flag |= CCACHE;

	return c;
}

Chan*
mntchan(void)
{
	Chan *c;

	c = devattach('M', 0);
	lock(&mntalloc);
	c->dev = mntalloc.id++;
	unlock(&mntalloc);

	return c;
}

void
mattach(Mnt *m, Chan *c, char *spec)
{
	ulong id;
	Mntrpc *r;

	r = mntralloc(0);
	c->mntptr = m;

	if(waserror()) {
		mntfree(r);
		nexterror();
	}

	r->request.type = Tattach;
	r->request.fid = c->fid;
	memmove(r->request.uname, up->user, NAMELEN);
	strncpy(r->request.aname, spec, NAMELEN);
	id = authrequest(m->c->session, &r->request);
	mountrpc(m, r);
	authreply(m->c->session, id, &r->reply);

	c->qid = r->reply.qid;
	c->mchan = m->c;
	c->mqid = c->qid;

	poperror();
	mntfree(r);
}

static Chan*
mntclone(Chan *c, Chan *nc)
{
	Mnt *m;
	Mntrpc *r;
	int alloc = 0;

	m = mntchk(c);
	r = mntralloc(c);
	if(nc == 0) {
		nc = newchan();
		alloc = 1;
	}
	if(waserror()) {
		mntfree(r);
		if(alloc)
			cclose(nc);
		nexterror();
	}

	r->request.type = Tclone;
	r->request.fid = c->fid;
	r->request.newfid = nc->fid;
	mountrpc(m, r);

	devclone(c, nc);
	nc->mqid = c->qid;
	incref(m);

	USED(alloc);
	poperror();
	mntfree(r);
	return nc;
}

static int
mntwalk(Chan *c, char *name)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		return 0;
	}
	r->request.type = Twalk;
	r->request.fid = c->fid;
	strncpy(r->request.name, name, NAMELEN);
	mountrpc(m, r);

	c->qid = r->reply.qid;

	poperror();
	mntfree(r);
	return 1;
}

static void
mntstat(Chan *c, char *dp)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Tstat;
	r->request.fid = c->fid;
	mountrpc(m, r);

	memmove(dp, r->reply.stat, DIRLEN);
	mntdirfix((uchar*)dp, c);
	poperror();
	mntfree(r);
}

static Chan*
mntopen(Chan *c, int omode)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Topen;
	r->request.fid = c->fid;
	r->request.mode = omode;
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->offset = 0;
	c->mode = openmode(omode);
	c->flag |= COPEN;
	poperror();
	mntfree(r);

	if(c->flag & CCACHE)
		copen(c);

	return c;
}

static void
mntcreate(Chan *c, char *name, int omode, ulong perm)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Tcreate;
	r->request.fid = c->fid;
	r->request.mode = omode;
	r->request.perm = perm;
	strncpy(r->request.name, name, NAMELEN);
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->flag |= COPEN;
	c->mode = openmode(omode);
	poperror();
	mntfree(r);

	if(c->flag & CCACHE)
		copen(c);
}

static void
mntclunk(Chan *c, int t)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()){
		mntfree(r);
		mclose(m, c);
		nexterror();
	}

	r->request.type = t;
	r->request.fid = c->fid;
	mountrpc(m, r);
	mntfree(r);
	mclose(m, c);
	poperror();
}

void
mclose(Mnt *m, Chan*)
{
	Mntrpc *q, *r;

	if(decref(m) != 0)
		return;

	for(q = m->queue; q; q = r) {
		r = q->list;
		q->flushed = 0;
		mntfree(q);
	}
	m->id = 0;
	cclose(m->c);
	mntpntfree(m);
}

void
mntpntfree(Mnt *m)
{
	Mnt *f, **l;

	lock(&mntalloc);
	l = &mntalloc.list;
	for(f = *l; f; f = f->list) {
		if(f == m) {
			*l = m->list;
			break;
		}
		l = &f->list;
	}

	m->list = mntalloc.mntfree;
	mntalloc.mntfree = m;
	unlock(&mntalloc);
}

static void
mntclose(Chan *c)
{
	mntclunk(c, Tclunk);
}

static void
mntremove(Chan *c)
{
	mntclunk(c, Tremove);
}

static void
mntwstat(Chan *c, char *dp)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Twstat;
	r->request.fid = c->fid;
	memmove(r->request.stat, dp, DIRLEN);
	mountrpc(m, r);
	poperror();
	mntfree(r);
}

long
mntread9p(Chan *c, void *buf, long n, vlong off)
{
	return mnt9prdwr(Tread, c, buf, n, off);
}

static long
mntread(Chan *c, void *buf, long n, vlong off)
{
	uchar *p, *e;
	int nc, cache, isdir;

	isdir = 0;
	cache = c->flag & CCACHE;
	if(c->qid.path & CHDIR) {
		cache = 0;
		isdir = 1;
	}

	p = buf;
	if(cache) {
		nc = cread(c, buf, n, off);
		if(nc > 0) {
			n -= nc;
			if(n == 0)
				return nc;
			p += nc;
			off += nc;
		}
		n = mntrdwr(Tread, c, p, n, off);
		cupdate(c, p, n, off);
		return n + nc;
	}

	n = mntrdwr(Tread, c, buf, n, off);
	if(isdir) {
		for(e = &p[n]; p < e; p += DIRLEN)
			mntdirfix(p, c);
	}

	return n;
}

long
mntwrite9p(Chan *c, void *buf, long n, vlong off)
{
	return mnt9prdwr(Twrite, c, buf, n, off);
}

static long
mntwrite(Chan *c, void *buf, long n, vlong off)
{
	return mntrdwr(Twrite, c, buf, n, off);
}

long
mnt9prdwr(int type, Chan *c, void *buf, long n, vlong off)
{
	Mnt *m;
 	ulong nr;
	Mntrpc *r;

	if(n > MAXRPC-32) {
		if(type == Twrite)
			error("write9p too long");
		n = MAXRPC-32;
	}

	m = mntchk(c);
	r = mntralloc(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = type;
	r->request.fid = c->fid;
	r->request.offset = off;
	r->request.data = buf;
	r->request.count = n;
	mountrpc(m, r);
	nr = r->reply.count;
	if(nr > r->request.count)
		nr = r->request.count;

	if(type == Tread)
		memmove(buf, r->reply.data, nr);

	poperror();
	mntfree(r);
	return nr;
}

long
mntrdwr(int type, Chan *c, void *buf, long n, vlong off)
{
	Mnt *m;
 	Mntrpc *r;
	char *uba;
	int cache;
	ulong cnt, nr, nreq;

	m = mntchk(c);
	uba = buf;
	cnt = 0;
	cache = c->flag & CCACHE;
	if(c->qid.path & CHDIR)
		cache = 0;
	for(;;) {
		r = mntralloc(c);
		if(waserror()) {
			mntfree(r);
			nexterror();
		}
		r->request.type = type;
		r->request.fid = c->fid;
		r->request.offset = off;
		r->request.data = uba;
		if(n > m->blocksize){
			if(c->qid.path & CHDIR)
				r->request.count = (m->blocksize/DIRLEN)*DIRLEN;
			else
				r->request.count = m->blocksize;
		} else
			r->request.count = n;
		mountrpc(m, r);
		nreq = r->request.count;
		nr = r->reply.count;
		if(nr > nreq)
			nr = nreq;

		if(type == Tread)
			memmove(uba, r->reply.data, nr);
		else if(cache)
			cwrite(c, (uchar*)uba, nr, off);

		poperror();
		mntfree(r);
		off += nr;
		uba += nr;
		cnt += nr;
		n -= nr;
		if(nr != nreq || n == 0 || up->nnote)
			break;
	}
	return cnt;
}

void
mountrpc(Mnt *m, Mntrpc *r)
{
	int t;

	r->reply.tag = 0;
	r->reply.type = 4;

	while(waserror()) {
		if((m->flags&MRECOV) == 0)
			nexterror();
		mntrecover(m, r);
	}
	mountio(m, r);
	poperror();

	t = r->reply.type;
	switch(t) {
	case Rerror:
		error(r->reply.ename);
	case Rflush:
		error(Eintr);
	default:
		if(t == r->request.type+1)
			break;
		print("mnt: proc %s %lud: mismatch rep 0x%lux T%d R%d rq %d fls %d rp %d\n",
			up->text, up->pid,
			r, r->request.type, r->reply.type, r->request.tag,
			r->flushtag, r->reply.tag);
		error(Emountrpc);
	}
}

void
mountio(Mnt *m, Mntrpc *r)
{
	int n;

	lock(m);
	r->flushed = 0;
	r->m = m;
	r->list = m->queue;
	m->queue = r;
	unlock(m);

	/* Transmit a file system rpc */
	n = convS2M(&r->request, r->rpc);
	if(n < 0)
		panic("bad message type in mountio");
	if(waserror()) {
		if(mntflush(m, r) == 0)
			nexterror();
	}
	else {
		if(devtab[m->c->type]->dc == L'M'){
			if(mnt9prdwr(Twrite, m->c, r->rpc, n, 0) != n)
				error(Emountrpc);
		}else{
			if(devtab[m->c->type]->write(m->c, r->rpc, n, 0) != n)
				error(Emountrpc);
		}
		r->stime = fastticks(nil);
		r->reqlen = n;
		poperror();
	}

	/* Gate readers onto the mount point one at a time */
	for(;;) {
		lock(m);
		if(m->rip == 0)
			break;
		unlock(m);
		if(waserror()) {
			if(mntflush(m, r) == 0)
				nexterror();
			continue;
		}
		sleep(&r->r, rpcattn, r);
		poperror();
		if(r->done)
			return;
	}
	m->rip = up;
	unlock(m);
	while(r->done == 0) {
		mntrpcread(m, r);
		mountmux(m, r);
	}
	mntgate(m);
}

void
mntrpcread(Mnt *m, Mntrpc *r)
{
	int n;

	for(;;) {
		if(waserror()) {
			if(mntflush(m, r) == 0) {
				mntgate(m);
				nexterror();
			}
			continue;
		}
		r->reply.type = 0;
		r->reply.tag = 0;
		if(devtab[m->c->type]->dc == L'M')
			n = mnt9prdwr(Tread, m->c, r->rpc, MAXRPC, 0);
		else
			n = devtab[m->c->type]->read(m->c, r->rpc, MAXRPC, 0);
		poperror();
		if(n == 0)
			continue;

		r->replen = n;
		if(convM2S(r->rpc, &r->reply, n) != 0)
			return;
	}
}

void
mntgate(Mnt *m)
{
	Mntrpc *q;

	lock(m);
	m->rip = 0;
	for(q = m->queue; q; q = q->list) {
		if(q->done == 0)
		if(wakeup(&q->r))
			break;
	}
	unlock(m);
}

void
mountmux(Mnt *m, Mntrpc *r)
{
	char *dp;
	Mntrpc **l, *q;

	lock(m);
	l = &m->queue;
	for(q = *l; q; q = q->list) {
		if(q->request.tag == r->reply.tag
		|| q->flushed && q->flushtag == r->reply.tag) {
			*l = q->list;
			unlock(m);
			if(q != r) {
				/*
				 * Completed someone else.
				 * Trade pointers to receive buffer.
				 */
				dp = q->rpc;
				q->rpc = r->rpc;
				r->rpc = dp;
				q->reply = r->reply;
				q->done = 1;
				if(mntstats != nil)
					(*mntstats)(q->request.type,
						m->c, q->stime,
						q->reqlen + r->replen);
				wakeup(&q->r);
			}else {
				if(mntstats != nil)
					(*mntstats)(r->request.type,
						m->c, r->stime,
						r->reqlen + r->replen);
				q->done = 1;
			}
			return;
		}
		l = &q->list;
	}
	unlock(m);
}

int
mntflush(Mnt *m, Mntrpc *r)
{
	int n, l;
	Fcall flush;

	lock(m);
	r->flushtag = m->flushtag++;
	if(m->flushtag == Tagend)
		m->flushtag = m->flushbase;
	r->flushed = 1;
	unlock(m);

	flush.type = Tflush;
	flush.tag = r->flushtag;
	flush.oldtag = r->request.tag;
	n = convS2M(&flush, r->flush);
	if(n < 0)
		panic("bad message type in mntflush");

	if(waserror()) {
		if(strcmp(up->error, Eintr) == 0)
			return 1;
		mntqrm(m, r);
		return 0;
	}
	l = devtab[m->c->type]->write(m->c, r->flush, n, 0);
	if(l != n)
		error(Ehungup);
	poperror();
	return 1;
}

Mntrpc*
mntralloc(Chan *c)
{
	Mntrpc *new;

	lock(&mntalloc);
	new = mntalloc.rpcfree;
	if(new == nil){
		new = malloc(sizeof(Mntrpc));
		if(new == nil) {
			unlock(&mntalloc);
			exhausted("mount rpc header");
		}
		/*
		 * The header is split from the data buffer as
		 * mountmux may swap the buffer with another header.
		 */
		new->rpc = mallocz(MAXRPC, 0);
		if(new->rpc == nil){
			free(new);
			unlock(&mntalloc);
			exhausted("mount rpc buffer");
		}
		new->request.tag = mntalloc.rpctag++;
	}
	else {
		mntalloc.rpcfree = new->list;
		mntalloc.nrpcfree--;
	}
	mntalloc.nrpcused++;
	unlock(&mntalloc);
	new->c = c;
	new->done = 0;
	new->flushed = 0;
	new->flushtag = 0;
	return new;
}

void
mntfree(Mntrpc *r)
{
	lock(&mntalloc);
	if(mntalloc.nrpcfree >= 10){
		free(r->rpc);
		free(r);
	}
	else{
		r->list = mntalloc.rpcfree;
		mntalloc.rpcfree = r;
		mntalloc.nrpcfree++;
	}
	mntalloc.nrpcused--;
	unlock(&mntalloc);
}

void
mntqrm(Mnt *m, Mntrpc *r)
{
	Mntrpc **l, *f;

	lock(m);
	r->done = 1;
	r->flushed = 0;

	l = &m->queue;
	for(f = *l; f; f = f->list) {
		if(f == r) {
			*l = r->list;
			break;
		}
		l = &f->list;
	}
	unlock(m);
}

void
recoverchan(Mnt*, Chan*)
{
panic("recoverchan");
#ifdef asdf
BUG: WON'T WORK WITH PATHS GONE?
	int i, n, flg;
	Path *safe, *p, **pav;

	if(m->c == 0)
		error(Eshutdown);

	flg = c->flag;
	/* Don't recursively recover */
	c->flag &= ~(COPEN|CRECOV);

	n = 0;
	for(p = c->path; p; p = p->parent)
		n++;
	pav = smalloc(sizeof(Path*)*n);
	i = n;
	for(p = c->path; p; p = p->parent)
		pav[--i] = p;

	safe = c->path;

	if(waserror()) {
		c->flag = flg;
		free(pav);
		nexterror();
	}

	/* Attach the fid onto the file server (sets c->path to #Mxxx) */
	mattach(m, c, c->xmh->mount->spec);
	poperror();

	/*
	 * c is now at the root so we free where
	 * the chan was before the server connection was lost
	 */
	decref(safe);

	for(i = 1; i < n; i++) {
		if(mntwalk(c, pav[i]->elem) == 0) {
			free(pav);
			/* Shut down the channel */
			c->dev = m->id-1;
			error(Erecover);
		}
	}
	free(pav);
	if(flg&COPEN)
		mntopen(c, c->mode);
#endif
}

Mnt*
mntchk(Chan *c)
{
	Mnt *m;

	m = c->mntptr;

	/*
	 * Was it closed and reused
	 */
	if(m->id == 0 || m->id >= c->dev)
		error(Eshutdown);

	/*
	 * Try and get the channel back after a crash
	 */
	if((c->flag&CRECOV) && m->recprog == 0)
		recoverchan(m, c);

	return m;
}

void
mntdirfix(uchar *dirbuf, Chan *c)
{
	int r;

	r = devtab[c->type]->dc;
	dirbuf[DIRLEN-4] = r>>0;
	dirbuf[DIRLEN-3] = r>>8;
	dirbuf[DIRLEN-2] = c->dev;
	dirbuf[DIRLEN-1] = c->dev>>8;
}

int
rpcattn(void *v)
{
	Mntrpc *r;

	r = v;
	return r->done || r->m->rip == 0;
}

int
recdone(void *v)
{
	Mnt *m;

	m = v;
	return m->recprog == 0;
}

void
mntrecdel(Mnt *m, Mntrpc *r)
{
	Mntrpc *f, **l;

	lock(m);
	l = &m->recwait;
	for(f = *l; f; f = f->list) {
		if(f == r) {
			*l = r->list;
			break;
		}
	}
	unlock(m);
}

void
mntrecover(Mnt *m, Mntrpc *r)
{
	char *ps;

	lock(m);
	if(m->recprog == 0) {
		m->recprog = 1;
		unlock(m);
		chanrec(m);
		/*
		 * Send a message to boot via #/recover
		 */
panic("mntrecover\n");
//		rootrecover(m->c->path, m->tree.root->elem);
		lock(m);
	}
	r->list = m->recwait;
	m->recwait = r;
	unlock(m);

	pprint("lost server connection, wait...\n");

	ps = up->psstate;
	up->psstate = "Recover";

	if(waserror()) {
		up->psstate = ps;
		mntrecdel(m, r);
		nexterror();
	}
	sleep(&r->r, recdone, m);
	poperror();

	r->done = 0;
	mntrecdel(m, r);
	if(r->c != 0)
		recoverchan(m, r->c);

	up->psstate = ps;
}

void
mntrepl(char*)
{
panic("mntrepl");
#ifdef asdf
	int fd;
	Mnt *m;
	char *p;
	Chan *c1, *c2;
	Mntrpc *r;

	/* reply from boot is 'fd #M23' */
	fd = strtoul(buf, &p, 0);

	p++;
	lock(&mntalloc);
	for(m = mntalloc.list; m; m = m->list) {
		if(strcmp(p, m->tree.root->elem) == 0)
			break;
	}
	unlock(&mntalloc);
	if(m == 0)
		error(Eunmount);

	c1 = fdtochan(fd, ORDWR, 0, 1);	/* error check and inc ref */

	/* If the channel was posted fix it up */
	srvrecover(m->c, c1);

	lock(m);
	c2 = m->c;
	m->c = c1;
	m->recprog = 0;

	/* Wakeup partially complete rpc */
	for(r = m->recwait; r; r = r->list)
		wakeup(&r->r);

	unlock(m);
	cclose(c2);
#endif
}

Dev mntdevtab = {
	'M',
	"mnt",

	mntreset,
	devinit,
	mntattach,
	mntclone,
	mntwalk,
	mntstat,
	mntopen,
	mntcreate,
	mntclose,
	mntread,
	devbread,
	mntwrite,
	devbwrite,
	mntremove,
	mntwstat,
};
