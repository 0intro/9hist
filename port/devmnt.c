#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#define MAXRPC (IOHDRSZ+8192)

struct Mntrpc
{
	Chan*	c;		/* Channel for whom we are working */
	Mntrpc*	list;		/* Free/pending list */
	Fcall	request;	/* Outgoing file system protocol message */
	Fcall reply;		/* Incoming reply */
	Mnt*	m;		/* Mount device during rpc */
	Rendez	r;		/* Place to hang out */
	uchar*	rpc;		/* I/O Data buffer */
	char	done;		/* Rpc completed */
	uvlong	stime;		/* start time for mnt statistics */
	ulong	reqlen;		/* request length for mnt statistics */
	ulong	replen;		/* reply length for mnt statistics */
	Mntrpc*	flushed;	/* message this one flushes */
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

void	mattach(Mnt*, Chan*, char*);
void	mntauth(Mnt*, Mntrpc*, char*, ushort);
Mnt*	mntchk(Chan*);
void	mntdirfix(uchar*, Chan*);
Mntrpc*	mntflushalloc(Mntrpc*, ulong);
void	mntflushfree(Mnt*, Mntrpc*);
void	mntfree(Mntrpc*);
void	mntgate(Mnt*);
void	mntpntfree(Mnt*);
void	mntqrm(Mnt*, Mntrpc*);
Mntrpc*	mntralloc(Chan*, ulong);
long	mntrdwr(int, Chan*, void*, long, vlong);
int	mntrpcread(Mnt*, Mntrpc*);
void	mountio(Mnt*, Mntrpc*);
void	mountmux(Mnt*, Mntrpc*);
void	mountrpc(Mnt*, Mntrpc*);
int	rpcattn(void*);
void	mclose(Mnt*, Chan*);
Chan*	mntchan(void);

char	Esbadstat[] = "invalid directory entry received from server";

void (*mntstats)(int, Chan*, uvlong, ulong);

enum
{
	Tagspace	= 1,
};

static void
mntreset(void)
{
	mntalloc.id = 1;
	mntalloc.rpctag = Tagspace;
	fmtinstall('F', fcallconv);
	fmtinstall('D', dirconv);
	fmtinstall('M', dirmodeconv);

	cinit();
}

static Chan*
mntattach(char *muxattach)
{
	Mnt *m;
	Chan *c;
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
	if(m->c->iounit == 0)
		m->c->iounit = MAXRPC;
	m->flags = bogus.flags & ~MCACHE;

	incref(m->c);

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
	Mntrpc *r;

	r = mntralloc(0, m->c->iounit);
	c->mntptr = m;

	if(waserror()) {
		mntfree(r);
		nexterror();
	}

	r->request.type = Tattach;
	r->request.fid = c->fid;
	r->request.uname = up->user;
	r->request.aname = spec;
	r->request.nauth = 0;
	r->request.auth = (uchar*)"";
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->mchan = m->c;
	c->mqid = c->qid;

	poperror();
	mntfree(r);
}

static Walkqid*
mntwalk(Chan *c, Chan *nc, char **name, int nname)
{
	int i, alloc;
	Mnt *m;
	Mntrpc *r;
	Walkqid *wq;

if(0){
	print("mntwalk ");
	for(i=0; i<nname; i++)
		print("%s/", name[i]);
	print("\n");
}

	if(nname > MAXWELEM)
		error("devmnt: too many name elements");
	alloc = 0;
	wq = smalloc(sizeof(Walkqid)+(nname-1)*sizeof(Qid));
	if(waserror()){
		if(alloc && wq->clone!=nil)
			cclose(wq->clone);
		free(wq);
		return nil;
	}

	alloc = 0;
	m = mntchk(c);
	r = mntralloc(c, m->c->iounit);
	if(nc == nil){
		nc = devclone(c);
		/*
		 * Until the other side accepts this fid, we can't mntclose it.
		 * Therefore set type to 0 for now; rootclose is known to be safe.
		 */
		nc->type = 0;
		alloc = 1;
	}
	wq->clone = nc;

	if(waserror()) {
		mntfree(r);
		nexterror();
		return nil;
	}
	r->request.type = Twalk;
	r->request.fid = c->fid;
	r->request.newfid = nc->fid;
	r->request.nwname = nname;
	memmove(r->request.wname, name, nname*sizeof(char*));

	mountrpc(m, r);

	if(r->reply.nwqid > nname)
		error("too many QIDs returned by walk");
	if(r->reply.nwqid < nname){
		if(alloc)
			cclose(nc);
		wq->clone = nil;
		if(r->reply.nwqid == 0){
			free(wq);
			wq = nil;
			goto Return;
		}
	}

	/* move new fid onto mnt device and update its qid */
	if(wq->clone != nil){
		if(wq->clone != c){
			wq->clone->type = c->type;
			incref(m);
		}
		if(r->reply.nwqid > 0)
			wq->clone->qid = r->reply.wqid[r->reply.nwqid-1];
	}
	wq->nqid = r->reply.nwqid;
	for(i=0; i<wq->nqid; i++)
		wq->qid[i] = r->reply.wqid[i];

    Return:
	poperror();
	mntfree(r);
	poperror();
	return wq;
}

static int
mntstat(Chan *c, uchar *dp, int n)
{
	Mnt *m;
	Mntrpc *r;

	if(n < BIT16SZ)
		error(Eshortstat);
	m = mntchk(c);
	r = mntralloc(c, m->c->iounit);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Tstat;
	r->request.fid = c->fid;
	mountrpc(m, r);

	if(r->reply.nstat >= 1<<(8*BIT16SZ))
		error("returned stat buffer count too large");

	if(r->reply.nstat > n){
		/* doesn't fit; just patch the count and return */
		PBIT16((uchar*)dp, r->reply.nstat);
		n = BIT16SZ;
	}else{
		n = r->reply.nstat;
		memmove(dp, r->reply.stat, n);
		validstat(dp, n);
		mntdirfix(dp, c);
	}
	poperror();
	mntfree(r);
	return n;
}

static Chan*
mntopencreate(int type, Chan *c, char *name, int omode, ulong perm)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c, m->c->iounit);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = type;
	r->request.fid = c->fid;
	r->request.mode = omode;
	if(type == Tcreate){
		r->request.perm = perm;
		r->request.name = name;
	}
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->offset = 0;
	c->mode = openmode(omode);
	c->iounit = r->reply.iounit;
	c->flag |= COPEN;
	poperror();
	mntfree(r);

	if(c->flag & CCACHE)
		copen(c);

	return c;
}

static Chan*
mntopen(Chan *c, int omode)
{
	return mntopencreate(Topen, c, nil, omode, 0);
}

static void
mntcreate(Chan *c, char *name, int omode, ulong perm)
{
	mntopencreate(Tcreate, c, name, omode, perm);
}

static void
mntclunk(Chan *c, int t)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c, m->c->iounit);
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

static int
mntwstat(Chan *c, uchar *dp, int n)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc(c, m->c->iounit);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Twstat;
	r->request.fid = c->fid;
	r->request.nstat = n;
	r->request.stat = dp;
	mountrpc(m, r);
	poperror();
	mntfree(r);
	return n;
}

static long
mntread(Chan *c, void *buf, long n, vlong off)
{
	uchar *p, *e;
	int nc, cache, isdir, dirlen;

	isdir = 0;
	cache = c->flag & CCACHE;
	if(c->qid.type & QTDIR) {
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
		for(e = &p[n]; p+BIT16SZ < e; p += dirlen){
			dirlen = BIT16SZ+GBIT16(p);
			if(p+dirlen > e)
				break;
			validstat(p, dirlen);
			mntdirfix(p, c);
		}
		if(p != e)
			error(Esbadstat);
	}

	return n;
}

static long
mntwrite(Chan *c, void *buf, long n, vlong off)
{
	return mntrdwr(Twrite, c, buf, n, off);
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
	if(c->qid.type & QTDIR)
		cache = 0;
	for(;;) {
		r = mntralloc(c, m->c->iounit);
		if(waserror()) {
			mntfree(r);
			nexterror();
		}
		r->request.type = type;
		r->request.fid = c->fid;
		r->request.offset = off;
		r->request.data = uba;
		nr = n;
		if(nr > m->c->iounit-IOHDRSZ)
			nr = m->c->iounit-IOHDRSZ;
		if(c->iounit != 0 && nr > c->iounit)
			nr = c->iounit;
		r->request.count = nr;
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
	char *sn, *cn;
	int t;

	r->reply.tag = 0;
	r->reply.type = Tmax;	/* can't ever be a valid message type */

	mountio(m, r);

	t = r->reply.type;
	switch(t) {
	case Rerror:
		error(r->reply.ename);
	case Rflush:
		error(Eintr);
	default:
		if(t == r->request.type+1)
			break;
		sn = "?";
		if(m->c->name != nil)
			sn = m->c->name->s;
		cn = "?";
		if(r->c != nil && r->c->name != nil)
			cn = r->c->name->s;
		print("mnt: proc %s %lud: mismatch from %s %s rep 0x%lux tag %d fid %d T%d R%d rp %d\n",
			up->text, up->pid, sn, cn,
			r, r->request.tag, r->request.fid, r->request.type,
			r->reply.type, r->reply.tag);
		error(Emountrpc);
	}
}

void
mountio(Mnt *m, Mntrpc *r)
{
	int n;

	while(waserror()) {
		if(m->rip == up)
			mntgate(m);
		if(strcmp(up->error, Eintr) != 0){
			mntflushfree(m, r);
			nexterror();
		}
		r = mntflushalloc(r, m->c->iounit);
	}

	lock(m);
	r->m = m;
	r->list = m->queue;
	m->queue = r;
	unlock(m);

	/* Transmit a file system rpc */
	if(m->c->iounit == 0)
		panic("iounit");
	n = convS2M(&r->request, r->rpc, m->c->iounit);
	if(n < 0)
		panic("bad message type in mountio");
	if(devtab[m->c->type]->write(m->c, r->rpc, n, 0) != n)
		error(Emountrpc);
	r->stime = fastticks(nil);
	r->reqlen = n;

	/* Gate readers onto the mount point one at a time */
	for(;;) {
		lock(m);
		if(m->rip == 0)
			break;
		unlock(m);
		sleep(&r->r, rpcattn, r);
		if(r->done){
			poperror();
			mntflushfree(m, r);
			return;
		}
	}
	m->rip = up;
	unlock(m);
	while(r->done == 0) {
		if(mntrpcread(m, r) < 0)
			error(Emountrpc);
		mountmux(m, r);
	}
	mntgate(m);
	poperror();
	mntflushfree(m, r);
}

int
mntreadn(Chan *c, uchar *buf, int n, int uninterruptable)
{
	int m, dm;

	for(m=0; m<n; m+=dm){
		if(uninterruptable)
			if(waserror()){
				/* user DEL may stop assembly; wait for full 9P msg. */
				if(strstr(up->error, "interrupt") == nil)
					nexterror();
				dm = 0;
				continue;
			}
		dm = devtab[c->type]->read(c, buf, n-m, 0);
		if(uninterruptable)
			poperror();
		if(dm <= 0)
			return 0;
		buf += dm;
	}
	return n;
}

int
mntrpcread(Mnt *m, Mntrpc *r)
{
	int n, len;
	ulong chunk;

	chunk = m->c->iounit;
	if(chunk == 0 || chunk > m->c->iounit)
		chunk = m->c->iounit;
	r->reply.type = 0;
	r->reply.tag = 0;
	/* read size, then read exactly the right number of bytes */
	n = mntreadn(m->c, r->rpc, BIT32SZ, 0);
	if(n != BIT32SZ){
		if(n > 0)
			print("devmnt expected BIT32SZ got %d\n", n);
		return -1;
	}
	len = GBIT32((uchar*)r->rpc);
	if(len <= BIT32SZ || len > chunk){
		print("devmnt: len %d max messagesize %ld\n", len, m->c->iounit);
		return -1;
	}
	n += mntreadn(m->c, r->rpc+BIT32SZ, len-BIT32SZ, 1);
	if(n != len){
		print("devmnt: length %d expected %d\n", n, len);
		return -1;
	}

	r->replen = n;
	if(convM2S(r->rpc, n, &r->reply) == 0){
		int i;
		print("bad conversion of received message; %d bytes iounit %ld\n", n, m->c->iounit);
		for(i=0; i<n; i++)
			print("%.2ux ", (uchar)r->rpc[i]);
		print("\n");
		return -1;
	}
	return 0;
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
	uchar *dp;
	Mntrpc **l, *q;

	lock(m);
	l = &m->queue;
	for(q = *l; q; q = q->list) {
		/* look for a reply to a message */
		if(q->request.tag == r->reply.tag) {
			*l = q->list;
			if(q != r) {
				/*
				 * Completed someone else.
				 * Trade pointers to receive buffer.
				 */
				dp = q->rpc;
				q->rpc = r->rpc;
				r->rpc = dp;
				q->reply = r->reply;
			}
			q->done = 1;
			unlock(m);
			if(mntstats != nil)
				(*mntstats)(q->request.type,
					m->c, q->stime,
					q->reqlen + r->replen);
			if(q != r)
				wakeup(&q->r);
			return;
		}
		l = &q->list;
	}
	unlock(m);
	print("unexpected reply tag %ud; type %d\n", r->reply.tag, r->reply.type);
}

/*
 * Create a new flush request and chain the previous
 * requests from it
 */
Mntrpc*
mntflushalloc(Mntrpc *r, ulong iounit)
{
	Mntrpc *fr;

	fr = mntralloc(0, iounit);

	fr->request.type = Tflush;
	if(r->request.type == Tflush)
		fr->request.oldtag = r->request.oldtag;
	else
		fr->request.oldtag = r->request.tag;
	fr->flushed = r;

	return fr;
}

/*
 *  Free a chain of flushes.  Remove each unanswered
 *  flush and the original message from the unanswered
 *  request queue.  Mark the original message as done
 *  and if it hasn't been answered set the reply to to
 *  Rflush.
 */
void
mntflushfree(Mnt *m, Mntrpc *r)
{
	Mntrpc *fr;

	while(r){
		fr = r->flushed;
		if(!r->done){
			r->reply.type = Rflush;
			mntqrm(m, r);
		}
		if(fr)
			mntfree(r);
		r = fr;
	}
}

Mntrpc*
mntralloc(Chan *c, ulong iounit)
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
		new->rpc = mallocz(iounit, 0);
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
	new->flushed = nil;
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

	return m;
}

/*
 * Rewrite channel type and dev for in-flight data to
 * reflect local values.  These entries are known to be
 * the first two in the Dir encoding after the count.
 */
void
mntdirfix(uchar *dirbuf, Chan *c)
{
	uint r;

	r = devtab[c->type]->dc;
	dirbuf += BIT16SZ;	/* skip count */
	PBIT16(dirbuf, r);
	dirbuf += BIT16SZ;
	PBIT32(dirbuf, c->dev);
}

int
rpcattn(void *v)
{
	Mntrpc *r;

	r = v;
	return r->done || r->m->rip == 0;
}

Dev mntdevtab = {
	'M',
	"mnt",

	mntreset,
	devinit,
	mntattach,
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
