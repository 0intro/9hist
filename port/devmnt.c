#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"fcall.h"

typedef struct Mnt	Mnt;
typedef struct Mnthdr	Mnthdr;
typedef struct MntQ	MntQ;

struct Mnt
{
	Ref;			/* for number of chans, incl. mntpt but not msg */
	ulong	mntid;		/* serial # */
	Chan	*mntpt;		/* channel in user's name space */
	MntQ	*q;
};

struct MntQ
{
	Ref;
	QLock;			/* for access */
	MntQ	*next;		/* for allocation */
	Chan	*msg;		/* for reading and writing messages */
	Proc	*reader;	/* process reading response */
	Mnthdr	*writer;	/* queue of headers of written messages */
};

#define	BITROUND 256
#define	BUFSIZE	(MAXFDATA+MAXMSG)
typedef struct Mntbuf Mntbuf;
struct Mntbuf
{
	Mntbuf	*next;
	char	buf[BUFSIZE+BITROUND]; 	/* BUG */
};

struct
{
	Lock;
	Mntbuf	*free;
}mntbufalloc;

struct Mnthdr
{
	Mnthdr	*next;		/* in free list or writers list */
	Mnthdr	*prev;		/* in writers list only */
	short	active;
	short	flushing;	/* a Tflush has been sent */
	Fcall	thdr;
	Fcall	rhdr;
	Rendez	r;
	Proc	*p;
	Mntbuf	*mbr;
	int	readreply;	/* true if we are reader or our reply has come */
};

struct
{
	Lock;
	Mnthdr	*arena;
	Mnthdr	*free;
}mnthdralloc;

struct
{
	Lock;
	QLock;
	MntQ	*arena;
	MntQ	*free;
}mntqalloc;

struct
{
	Lock;
	long	id;
}mntid;

Mnt	*mnt;
void	mntxmit(Mnt*, Mnthdr*);

Mntbuf*
mballoc(void)
{
	Mntbuf *mb;

loop:
	lock(&mntbufalloc);
	if(mb = mntbufalloc.free){		/* assign = */
		mntbufalloc.free = mb->next;
		unlock(&mntbufalloc);
		return mb;
	}
	unlock(&mntbufalloc);
	print("no mntbufs\n");
	if(u == 0)
		panic("mballoc");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
mbfree(Mntbuf *mb)
{
	lock(&mntbufalloc);
	mb->next = mntbufalloc.free;
	mntbufalloc.free = mb;
	unlock(&mntbufalloc);
}

Mnthdr*
mhalloc(void)
{
	Mnthdr *mh;

loop:
	lock(&mnthdralloc);
	if(mh = mnthdralloc.free){		/* assign = */
		mnthdralloc.free = mh->next;
if(mh->active) print("mh->active\n");
if(mh->flushing) print("mh->flushing\n");
if(mh->mbr) print("mh->mbr\n");
		mh->mbr = 0;
		unlock(&mnthdralloc);
		return mh;
	}
	unlock(&mnthdralloc);
	print("no mnthdrs\n");
	if(u == 0)
		panic("mhalloc");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
mhfree(Mnthdr *mh)
{
	if(mh->flushing)
		return;
	mh->active = 0;
	lock(&mnthdralloc);
	mh->next = mnthdralloc.free;
	mnthdralloc.free = mh;
	unlock(&mnthdralloc);
}

MntQ*
mqalloc(Chan *msg)	/* mntqalloc is qlocked */
{
	MntQ *q;

	if(q = mntqalloc.free){		/* assign = */
		mntqalloc.free = q->next;
		lock(q);
		q->ref = 1;
		q->msg = msg;
		unlock(q);
		incref(msg);
		q->writer = 0;
		q->reader = 0;
		return q;
	}
	panic("no mntqs\n");			/* there MUST be enough */
}

void
mqfree(MntQ *mq)
{
	Chan *msg = 0;

	lock(mq);
	if(--mq->ref == 0){
		msg = mq->msg;
		mq->msg = 0;
		lock(&mntqalloc);
		mq->next = mntqalloc.free;
		mntqalloc.free = mq;
		unlock(&mntqalloc);
	}
	unlock(mq);
	if(msg)		/* after locks are down */
		close(msg);
}

Mnt*
mntdev(Chan *c, int noerr)
{
	Mnt *m;
	int i;

	m = &mnt[c->mntindex];
	if(m->mntid==c->dev && m->q!=0)
		return m;
	if(noerr)
		return 0;
	print("mntdev shutdown %d %d %d %lux\n", c->dev, c->mntindex,
			m->mntid, m->q);
	error(Eshutdown);
}

void
mntreset(void)
{
	int i;
	Mntbuf *mb;
	Mnthdr *mh;
	MntQ *mq;

	mnt = ialloc(conf.nmntdev*sizeof(Mnt), 0);

	mb = ialloc(conf.nmntbuf*sizeof(Mntbuf), 0);
	for(i=0; i<conf.nmntbuf-1; i++)
		mb[i].next = &mb[i+1];
	mb[i].next = 0;
	mntbufalloc.free = mb;

	mh = ialloc(conf.nmnthdr*sizeof(Mnthdr), 0);
	for(i=0; i<conf.nmnthdr-1; i++){
		mh[i].next = &mh[i+1];
		mh[i].thdr.tag = i;
	}
	mh[i].next = 0;
	mh[i].thdr.tag = i;
	mnthdralloc.arena = mh;
	mnthdralloc.free = mh;

	mq = ialloc(conf.nmntdev*sizeof(MntQ), 0);
	for(i=0; i<conf.nmntdev-1; i++)
		mq[i].next = &mq[i+1];
	mq[i].next = 0;
	mntqalloc.arena = mq;
	mntqalloc.free = mq;
}

void
mntinit(void)
{
}

Chan*
mntattach(char *crud)
{
	int i;
	Mnt *m, *mm;
	Mnthdr *mh;
	MntQ *q;
	Chan *c, *cm;
	struct bogus{
		Chan	*chan;
		char	*spec;
		char	*auth;
	}bogus;

	bogus = *((struct bogus *)crud);

	m = mnt;
	for(i=0; i<conf.nmntdev; i++,m++){
		lock(m);
		if(m->ref == 0)
			goto Found;
		unlock(m);
	}
	error(Enomntdev);

    Found:
	m->ref = 1;
	unlock(m);
	lock(&mntid);
	m->mntid = ++mntid.id;
	unlock(&mntid);
	c = devattach('M', bogus.spec);
	c->dev = m->mntid;
	c->mntindex = m-mnt;
	m->mntpt = c;
	cm = bogus.chan;

	/*
	 * Look for queue to same msg channel
	 */
	q = mntqalloc.arena;
	qlock(&mntqalloc);
	for(i=0; i<conf.nmntdev; i++,q++)
		if(q->msg==cm){
			lock(q);
			if(q->ref && q->msg==cm){
				m->q = q;
				q->ref++;
				unlock(q);
				goto out;
			}
			unlock(q);
		}
	m->q = mqalloc(cm);

    out:
	qunlock(&mntqalloc);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		close(c);
		nexterror();
	}
	mh->thdr.type = Tattach;
	mh->thdr.fid = c->fid;
	memmove(mh->thdr.uname, u->p->pgrp->user, NAMELEN);
	strcpy(mh->thdr.aname, bogus.spec);
	strcpy(mh->thdr.auth, bogus.auth);
	mntxmit(m, mh);
	c->qid = mh->rhdr.qid;
	c->mchan = m->q->msg;
	c->mqid = c->qid;
	mhfree(mh);
	poperror();
	return c;
}

Chan*
mntclone(Chan *c, Chan *nc)
{
	Mnt *m;
	Mnthdr *mh;
	int new;

	new = 0;
	if(nc == 0){
		nc = newchan();
		new = 1;
		if(waserror()){
			close(nc);
			nexterror();
		}
	}
	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tclone;
	mh->thdr.fid = c->fid;
	mh->thdr.newfid = nc->fid;
	mntxmit(m, mh);
	nc->type = c->type;
	nc->dev = c->dev;
	nc->qid = c->qid;
	nc->mode = c->mode;
	nc->flag = c->flag;
	nc->offset = c->offset;
	nc->mnt = c->mnt;
	nc->mountid = c->mountid;
	nc->aux = c->aux;
	nc->mntindex = c->mntindex;
	nc->mchan = c->mchan;
	nc->mqid = c->qid;
	mhfree(mh);
	poperror();
	if(new)
		poperror();
	incref(m);
	return nc;
}

int	 
mntwalk(Chan *c, char *name)
{
	Mnt *m;
	Mnthdr *mh;
	int found;

	found = 1;
	m = mntdev(c, 0);
	mh = mhalloc();
	mh->thdr.type = Twalk;
	mh->thdr.fid = c->fid;
	strcpy(mh->thdr.name, name);
	if(waserror()){	/* BUG: can check type of error? */
		found = 0;
		goto Out;
	}
	mntxmit(m, mh);
	c->qid = mh->rhdr.qid;
	poperror();
    Out:
	mhfree(mh);
	return found;
}

void	 
mntstat(Chan *c, char *dp)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tstat;
	mh->thdr.fid = c->fid;
	mntxmit(m, mh);
	memmove(dp, mh->rhdr.stat, DIRLEN);
	dp[DIRLEN-4] = devchar[c->type];
	dp[DIRLEN-3] = 0;
	dp[DIRLEN-2] = c->dev;
	dp[DIRLEN-1] = c->dev>>8;
	mhfree(mh);
	poperror();
}

Chan*
mntopen(Chan *c, int omode)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Topen;
	mh->thdr.fid = c->fid;
	mh->thdr.mode = omode;
	mntxmit(m, mh);
	c->qid = mh->rhdr.qid;
	mhfree(mh);
	poperror();
	c->offset = 0;
	c->mode = openmode(omode);
	c->flag |= COPEN;
	return c;
}

void	 
mntcreate(Chan *c, char *name, int omode, ulong perm)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tcreate;
	mh->thdr.fid = c->fid;
	strcpy(mh->thdr.name, name);
	mh->thdr.mode = omode;
	mh->thdr.perm = perm;
	mntxmit(m, mh);
	c->qid = mh->rhdr.qid;
	mhfree(mh);
	poperror();
	c->flag |= COPEN;
	c->mode = openmode(omode);
	c->qid = mh->rhdr.qid;
}

void	 
mntclunk(Chan *c, int t)
{
	Mnt *m;
	Mnthdr *mh;
	MntQ *q;
	int waserr;

	m = mntdev(c, 0);
	mh = mhalloc();
	mh->thdr.type = t;
	mh->thdr.fid = c->fid;
	waserr = 0;
	if(waserror())		/* gotta clean up as if there wasn't */
		waserr = 1;
	else
		mntxmit(m, mh);
	mhfree(mh);
	if(c == m->mntpt)
		m->mntpt = 0;
	lock(m);
	if(--m->ref == 0){		/* BUG: need to hang up all pending i/o */
		q = m->q;
		m->q = 0;
		m->mntid = 0;
		unlock(m);		/* mqfree can take time */
		mqfree(q);
	}else
		unlock(m);
	if(waserr)
		nexterror();
	poperror();
}

void
mntclose(Chan *c)
{
	mntclunk(c, Tclunk);
}

long
mntreadwrite(Chan *c, void *vbuf, long n, int type, ulong offset)
{
	Mnt *m;
	Mnthdr *mh;
	long nt, nr, count;
	char *buf;

	buf = vbuf;
	count = 0;
	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = type;
	mh->thdr.fid = c->fid;
    Loop:
	nt = n;
	if(nt > MAXFDATA)
		nt = MAXFDATA;
	mh->thdr.offset = offset;
	mh->thdr.count = nt;
	mh->thdr.data = buf;
	mntxmit(m, mh);
	nr = mh->rhdr.count;
	offset += nr;
	count += nr;
	buf += nr;
	n -= nr;
	if(n && nr==nt)
		goto Loop;
	mhfree(mh);
	poperror();
	return count;
}

long	 
mntread(Chan *c, void *buf, long n, ulong offset)
{
	long i;
	uchar *b;

	n = mntreadwrite(c, buf, n, Tread, offset);
	if(c->qid.path & CHDIR){
		b = (uchar*)buf;
		for(i=n-DIRLEN; i>=0; i-=DIRLEN){
			b[DIRLEN-4] = devchar[c->type];
			b[DIRLEN-3] = 0;
			b[DIRLEN-2] = c->dev;
			b[DIRLEN-1] = c->dev>>8;
			b += DIRLEN;
		}
	}
	return n;
}

long	 
mntwrite(Chan *c, void *buf, long n, ulong offset)
{
	return mntreadwrite(c, buf, n, Twrite, offset);
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
	Mnthdr *mh;

	m = mntdev(c, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Twstat;
	mh->thdr.fid = c->fid;
	memmove(mh->thdr.stat, dp, DIRLEN);
	mntxmit(m, mh);
	mhfree(mh);
	poperror();
}

void
mntwunlink(MntQ *q, Mnthdr *w)		/* queue is locked and w is a writer */
{
	if(w->next)
		w->next->prev = w->prev;
	if(w->prev)
		w->prev->next = w->next;
	else{
		q->writer = w->next;
		if(q->writer)
			q->writer->prev = 0;
	}
}

/*
 * m->q is unlocked.  Send Tflush message to flush omh->tag.
 * Cut off all errors.   Caller will free omh
 */
void
mntflush(Mnt *m, Mnthdr *omh)	/* queue is unlocked */
{
	Mnthdr *mh;

	if(omh->thdr.type == Tflush){
		omh->flushing = 0;
		return;
	}

	mh = mhalloc();
	if(waserror()){
		omh->flushing = 0;
		mhfree(mh);
		return;		/* no more errors please */
	}
	mh->thdr.type = Tflush;
	mh->thdr.oldtag = omh->thdr.tag;
	mntxmit(m, mh);
	omh->flushing = 0;
	mhfree(mh);
	poperror();
}

void
mnterrdequeue(Mnt *m, Mnthdr *mh)	/* queue is unlocked */
{
	Mnthdr *w;
	MntQ *q;

	mh->flushing = 1;
	q = m->q;
	qlock(q);
	/* take self from queue if necessary */
	if(q->reader == u->p){	/* advance a writer to reader */
		w = q->writer;
		if(w){
			mntwunlink(q, w);
			q->reader = w->p;
			wakeup(&w->r);
		}else{
			q->reader = 0;
			q->writer = 0;
		}
	}else
		mntwunlink(q, mh);
	qunlock(q);
	mntflush(m, mh);
}

int
mntreadreply(void *a)
{
	return ((Mnthdr *)a)->readreply;
}

void
mntxmit(Mnt *m, Mnthdr *mh)
{
	ulong n;
	Mntbuf *mbw;
	Mnthdr *w, *ow;
	MntQ *q;
	int qlocked, tag, written;

	mh->mbr = 0;
	mbw = mballoc();
	if(waserror()){			/* 1 */
		if(mh->mbr){
			mbfree(mh->mbr);
			mh->mbr = 0;
		}
		mbfree(mbw);
		nexterror();
	}
	n = convS2M(&mh->thdr, mbw->buf);
	q = m->q;
	if(q == 0)
		error(Eshutdown);
#ifdef	BIT3
	/*
	 * Bit3 and Hotrod do their own multiplexing.  (Well, the file server does.)
	 * The code is different enough that it's broken out separately here.
	 */
	if(devchar[q->msg->type]!='3' && devchar[q->msg->type]!='H')
		goto Normal;

	incref(q);
	if(waserror()){		/* 2 */
		mqfree(q);
		nexterror();
	}
	if((*devtab[q->msg->type].write)(q->msg, mbw->buf, n, 0) != n){
		print("short write in mntxmit\n");
		error(Eshortmsg);
	}

	/*
	 * Read response
	 */
	if(waserror()){		/* 3 */
		mntflush(m, mh);
		nexterror();
	}
	mh->mbr = mballoc();
	n = (*devtab[q->msg->type].read)(q->msg, mh->mbr->buf, BUFSIZE, 0);
	poperror();		/* 3 */
	mqfree(q);
	poperror();		/* 2 */

	if(convM2S(mh->mbr->buf, &mh->rhdr, n) == 0){
		print("format error in mntxmit\n");
		error(Ebadmsg);
	}

	/*
	 * Various checks
	 */
	if(mh->rhdr.tag != mh->thdr.tag){
		print("tag mismatch %d %d\n", mh->rhdr.tag, mh->thdr.tag);
		error(Ebadmsg);
	}
	if(mh->rhdr.type == Rerror){
		if(m->mntpt)
			errors(mh->rhdr.ename);
		error(Eshutdown);
	}
	if(mh->rhdr.type != mh->thdr.type+1){
		print("type mismatch %d %d\n", mh->rhdr.type, mh->thdr.type+1);
		error(Ebadmsg);
	}
	if(mh->rhdr.fid != mh->thdr.fid){
		print("fid mismatch %d %d type %d\n", mh->rhdr.fid, mh->thdr.fid, mh->rhdr.type);
		error(Ebadmsg);
	}

	/*
	 * Copy out on read
	 */
	if(mh->thdr.type == Tread)
		memmove(mh->thdr.data, mh->rhdr.data, mh->rhdr.count);
	mbfree(mh->mbr);
	mh->mbr = 0;
	mbfree(mbw);
	poperror();		/* 1 */
	return;

    Normal:
#endif
	incref(q);
	qlock(q);
	qlocked = 1;
	if(waserror()){		/* 2 */
		if(qlocked)
			qunlock(q);
		mqfree(q);
		nexterror();
	}
	mh->readreply = 0;
	mh->active = 1;
	if((*devtab[q->msg->type].write)(q->msg, mbw->buf, n, 0) != n){
		print("short write in mntxmit\n");
		error(Eshortmsg);
	}
	if(q->reader == 0){		/* i will read */
		q->reader = u->p;
    Read:
		USED(qlocked);
		qunlock(q);
		qlocked = 0;
		if(waserror()){		/* 3 */
			mnterrdequeue(m, mh);
			nexterror();
		}
		mh->mbr = mballoc();
		do{
			n = (*devtab[q->msg->type].read)(q->msg, mh->mbr->buf, BUFSIZE, 0);
		}while(n == 0);
		poperror();		/* 3 */
		if(convM2S(mh->mbr->buf, &mh->rhdr, n) == 0){
			if(1){	/* BUG? IS THIS RIGHT? IGNORE AND RETRY */
				print(" MR ");
				qlock(q);
				qlocked = 1;
				goto FreeRead;
			}else{
				mnterrdequeue(m, mh);
				error(Ebadmsg);
			}
		}
		/*
		 * Response might not be mine
		 */
		USED(qlocked);
		qlock(q);
		qlocked = 1;
		tag = mh->rhdr.tag;
		if(tag == mh->thdr.tag){	/* it's mine */
			if(mh->rhdr.type != Rerror)
			if(mh->rhdr.type != mh->thdr.type+1){
				print(" T%c ", devchar[m->q->msg->type]);
				goto FreeRead;
			}
			q->reader = 0;
			if(w = q->writer){	/* advance a writer to reader */
				mntwunlink(q, w);
				q->reader = w->p;
				w->readreply = 1;
				wakeup(&w->r);
			}
			mh->active = 0;
			USED(qlocked);
			qunlock(q);
			qlocked = 0;
			goto Respond;
		}
		/*
		 * Hand response to correct recipient
		 */
		if(tag<0 || tag>=conf.nmnthdr){
			print("unknown tag %d\n", tag);
	FreeRead:
			mbfree(mh->mbr);
			mh->mbr = 0;
			goto Read;
		}
		w = &mnthdralloc.arena[tag];
		if(w->flushing || !w->active)	/* nothing to do; mntflush will clean up */
			goto FreeRead;
		if(mh->rhdr.type != Rerror)
		if(mh->rhdr.type != w->thdr.type+1){
			print(" t%c ", devchar[m->q->msg->type]);
			goto FreeRead;
		}
		w->mbr = mh->mbr;
		mh->mbr = 0;
		memmove(&w->rhdr, &mh->rhdr, sizeof mh->rhdr);
		mntwunlink(q, w);
		w->readreply = 1;
		wakeup(&w->r);
		goto Read;
	}else{
		mh->p = u->p;
		/* put self in queue */
		mh->next = q->writer;
		mh->prev = 0;
		if(q->writer)
			q->writer->prev = mh;
		q->writer = mh;
		qunlock(q);
		qlocked = 0;
		if(waserror()){		/* interrupted sleep */
			mnterrdequeue(m, mh);
			nexterror();
		}
		sleep(&mh->r, mntreadreply, mh);
		poperror();
		USED(qlocked);
		qlock(q);
		qlocked = 1;
		if(q->reader == u->p)	/* i got promoted */
			goto Read;
		mh->active = 0;
		USED(qlocked);
		qunlock(q);
		qlocked = 0;
		goto Respond;
	}

    Respond:
	mqfree(q);
	poperror();		/* 2 */
	if(mh->rhdr.type == Rerror){
		if(m->mntpt)
			errors(mh->rhdr.ename);
		error(Eshutdown);
	}else if(mh->rhdr.type != mh->thdr.type+1){
		print("bad type %d not %d in mntxmit\n", mh->rhdr.type, mh->thdr.type+1);
/*XXX*/		print("chan %c %d %lux %lux\n", devchar[m->q->msg->type],
				m->q->msg->dev, m->q->msg->qid.path,
				m->q->msg->stream);	
		error(Ebadmsg);
	}
	/*
	 * Copy out on read
	 */
	if(mh->thdr.type == Tread){
		if(mh->rhdr.count > mh->thdr.count)
			error(Ebadcnt);
		memmove(mh->thdr.data, mh->rhdr.data, mh->rhdr.count);
	}
	mbfree(mh->mbr);
	mh->mbr = 0;
	mbfree(mbw);
	USED(qlocked);
	poperror();		/* 1 */
}

void
mntdump(void)
{
	int i;
	MntQ *q;
	Mnthdr *h;
	Proc *p;

	for(i=0; i<conf.nmntdev; i++){
		q = &mntqalloc.arena[i];
		if(!q->msg)
			continue;
		p = q->reader;
		print("q rdr %d wrtr ", p? p->pid : 0);
		for(h=q->writer; h; h=h->next)
			print("(%lux %lux %d)", h, &h->r, (p=h->p)? p->pid : 0);
		print("\n");
	}
}
