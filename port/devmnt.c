#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"

struct Mntrpc
{
	Chan	*c;		/* Channel for whom we are working */
	Mntrpc	*list;		/* Free/pending list */
	Fcall	request;	/* Outgoing file system protocol message */
	Fcall	reply;		/* Incoming reply */
	Mnt	*m;		/* Mount device during rpc */
	Rendez	r;		/* Place to hang out */
	char	*rpc;		/* I/O Data buffer */
	char	done;		/* Rpc completed */
	char	flushed;	/* Flush was sent */
	ushort	flushtag;	/* Tag flush sent on */
	char	flush[MAXMSG];	/* Somewhere to build flush */
};

struct Mntalloc
{
	Lock;
	Mnt	*list;		/* Mount devices in use */
	Mnt	*mntfree;	/* Free list */
	Mntrpc	*rpcfree;
	int	id;
	int	rpctag;
}mntalloc;

#define MAXRPC		(MAXFDATA+MAXMSG)
#define limit(n, max)	(n > max ? max : n)

void	mattach(Mnt*, Chan*, char*);
void	mntauth(Mnt *, Mntrpc *, char *, ushort);
Mnt*	mntchk(Chan*);
void	mntdirfix(uchar*, Chan*);
int	mntflush(Mnt*, Mntrpc*);
void	mntfree(Mntrpc*);
void	mntgate(Mnt*);
void	mntpntfree(Mnt*);
void	mntqrm(Mnt*, Mntrpc*);
Mntrpc*	mntralloc(Chan*);
long	mntrdwr(int , Chan*, void*,long , ulong);
void	mntrpcread(Mnt*, Mntrpc*);
void	mountio(Mnt*, Mntrpc*);
void	mountmux(Mnt*, Mntrpc*);
void	mountrpc(Mnt*, Mntrpc*);
int	rpcattn(Mntrpc*);
void	mclose(Mnt*, Chan*);
void	mntrecover(Mnt*, Mntrpc*);
Chan*	mntchan(void);

enum
{
	Tagspace	= 1,
	Tagfls		= 0x8000,
	Tagend		= 0xfffe,
};

void
mntreset(void)
{
	mntalloc.id = 1;
	mntalloc.rpctag = Tagspace;

	cinit();
}

void
mntinit(void)
{
}

Chan*
mntattach(char *muxattach)
{
	Mnt *m;
	int nest;
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
				unlock(&mntalloc);
				m->ref++;
				unlock(m);
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
	lock(m);
	unlock(&mntalloc);

	m->ref = 1;
	m->queue = 0;
	m->rip = 0;
	m->c = c;
	m->c->flag |= CMSG;
	m->blocksize = MAXFDATA;	/**/
	m->flags = bogus.flags & ~MCACHE;

	switch(devchar[m->c->type]) {
	default:
		m->mux = 0;
		break;
	case 'C':			/* Cyclone */
		m->mux = 1;
		break;
	}
	incref(m->c);

	sprint(buf, "#M%d", m->id);
	m->tree.root = ptenter(&m->tree, 0, buf);

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

	/* Work out how deep we are nested and reduce the blocksize
	 * accordingly
	 */
	nest = 0;
	mc = m->c;
	while(mc) {
		if(mc->type != devno('M', 0))
			break;
		nest++;
		if(mc->mntptr == 0)
			break;
		mc = mc->mntptr->c;
	}
	m->blocksize -= nest * 32;

	/*
	 * Detect a recursive mount for a mount point served by exportfs.
	 * If CHDIR is clear in the returned qid the foreign server is
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
		c->path = c->mntptr->tree.root;
		incref(c->path);
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

	if(waserror()){
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
	c->path = m->tree.root;
	incref(c->path);

	poperror();
	mntfree(r);
}

Chan*
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
			close(nc);
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

int	 
mntwalk(Chan *c, char *name)
{
	Mnt *m;
	Path *op;
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
	op = c->path;
	c->path = ptenter(&m->tree, op, name);

	decref(op);

	poperror();
	mntfree(r);
	return 1;
}

void	 
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

Chan*
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

void	 
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

void	 
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
mclose(Mnt *m, Chan *c)
{
	Mntrpc *q, *r;

	if(decref(m) != 0)
		return;

	c->path = 0;
	ptclose(&m->tree);

	for(q = m->queue; q; q = r) {
		r = q->list;
		q->flushed = 0;
		mntfree(q);
	}
	m->id = 0;
	close(m->c);
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

void
mntclose(Chan *c)
{
	mntclunk(c, Tclunk);
}

void	 
mntremove(Chan *c)
{
	mntclunk(c, Tremove);
}

void
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
mntread(Chan *c, void *buf, long n, ulong offset)
{
	uchar *p, *e;
	int nc, cached;

	cached = c->flag & CCACHE;

	SET(nc);
	if(cached) {
		nc = cread(c, buf, n, offset);
		if(nc > 0) {
			n -= nc;
			if(n == 0)
				return nc;
			buf = (uchar*)buf+nc;
			offset += nc;
		}
	}

	n = mntrdwr(Tread, c, buf, n, offset);
	if(c->qid.path & CHDIR) {
		for(p = (uchar*)buf, e = &p[n]; p < e; p += DIRLEN)
			mntdirfix(p, c);
	}
	else if(cached) {
		cupdate(c, buf, n, offset);
		n += nc;
	}

	return n;
}

long	 
mntwrite(Chan *c, void *buf, long n, ulong offset)
{
	return mntrdwr(Twrite, c, buf, n, offset);
}

long
mntrdwr(int type, Chan *c, void *buf, long n, ulong offset)
{
	Mnt *m;
 	Mntrpc *r;
	char *uba;
	int cache;
	ulong cnt, nr;

	m = mntchk(c);
	uba = buf;
	cnt = 0;
	cache = c->flag & CCACHE;
	for(;;) {
		r = mntralloc(c);
		if(waserror()) {
			mntfree(r);
			nexterror();
		}
		r->request.type = type;
		r->request.fid = c->fid;
		r->request.offset = offset;
		r->request.data = uba;
		r->request.count = limit(n, m->blocksize);
		mountrpc(m, r);
		nr = r->reply.count;
		if(nr > r->request.count)
			nr = r->request.count;

		if(type == Tread)
			memmove(uba, r->reply.data, nr);
		else if(cache)
			cwrite(c, (uchar*)uba, nr, offset);

		poperror();
		mntfree(r);
		offset += nr;
		uba += nr;
		cnt += nr;
		n -= nr;
		if(nr != r->request.count || n == 0 || up->nnote)
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
		print("mnt: mismatch rep 0x%lux T%d R%d rq %d fls %d rp %d\n",
			r, t, r->reply.type, r->request.tag, 
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
	if(waserror()) {
		if(mntflush(m, r) == 0)
			nexterror();
	}
	else {
		if((*devtab[m->c->type].write)(m->c, r->rpc, n, 0) != n)
			error(Emountrpc);
		poperror();
	}
	if(m->mux) {
		mntqrm(m, r);
		mntrpcread(m, r);
		return;
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
	int n, zrd;

	zrd = 0;
	for(;;) {
		if(waserror()) {
			if(mntflush(m, r) == 0) {
				if(m->mux == 0)
					mntgate(m);
				nexterror();
			}
			continue;
		}
		r->reply.type = 0;
		r->reply.tag = 0;
		n = (*devtab[m->c->type].read)(m->c, r->rpc, MAXRPC, 0);
		poperror();
		if(n == 0) {
			if(++zrd > 3)
				error(Ehungup);
			continue;
		}

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
		if(q->done == 0) {
			lock(&q->r);
			if(q->r.p) {
				unlock(&q->r);
				unlock(m);
				wakeup(&q->r);
				return;
			}
			unlock(&q->r);
		}
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
			if(q != r) {		/* Completed someone else */
				dp = q->rpc;
				q->rpc = r->rpc;
				r->rpc = dp;
				q->reply = r->reply;
				q->done = 1;
				wakeup(&q->r);
			}else
				q->done = 1;
			return;
		}
		l = &q->list;
	}
	unlock(m);
}

int
mntflush(Mnt *m, Mntrpc *r)
{
	int n;
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

	if(waserror()) {
		if(strcmp(up->error, Eintr) == 0)
			return 1;
		mntqrm(m, r);
		return 0;
	}
	(*devtab[m->c->type].write)(m->c, r->flush, n, 0);
	poperror();
	return 1;
}

Mntrpc *
mntralloc(Chan *c)
{
	Mntrpc *new;

	lock(&mntalloc);
	new = mntalloc.rpcfree;
	if(new != 0)
		mntalloc.rpcfree = new->list;
	else {
		new = xalloc(sizeof(Mntrpc)+MAXRPC);
		if(new == 0) {
			unlock(&mntalloc);
			exhausted("mount rpc buffer");
		}
		new->rpc = (char*)new+sizeof(Mntrpc);
		new->request.tag = mntalloc.rpctag++;
	}
	unlock(&mntalloc);
	new->c = c;
	new->done = 0;
	new->flushed = 0;
	return new;
}

void
mntfree(Mntrpc *r)
{
	lock(&mntalloc);
	r->list = mntalloc.rpcfree;
	mntalloc.rpcfree = r;
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
recoverchan(Mnt *m, Chan *c)
{
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
	mattach(m, c, c->xmnt->spec);
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
}

Mnt *
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
	dirbuf[DIRLEN-4] = devchar[c->type]>>0;
	dirbuf[DIRLEN-3] = devchar[c->type]>>8;
	dirbuf[DIRLEN-2] = c->dev;
	dirbuf[DIRLEN-1] = c->dev>>8;
}

int
rpcattn(Mntrpc *r)
{
	return r->done || r->m->rip == 0;
}

int
recdone(Mnt *m)
{
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
		rootrecover(m->c->path, m->tree.root->elem);
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
	recoverchan(m, r->c);

	up->psstate = ps;
}

void
mntrepl(char *buf)
{
	int fd;
	Mnt *m;
	char *p;
	Chan *c1;
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
	close(m->c);
	m->c = c1;
	m->recprog = 0;

	/* Wakeup partially complete rpc */
	for(r = m->recwait; r; r = r->list)
		wakeup(&r->r);

	unlock(m);
}
