#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"fcall.h"

/*
 * Easy version: multiple sessions but no intra-session multiplexing, copy the data
 */

typedef struct Mnt	Mnt;
struct Mnt
{
	Ref;			/* for number of chans, incl. mntpt but not msg */
	QLock;			/* for access */
	ulong	mntid;		/* serial # */
	Chan	*msg;		/* for reading and writing messages */
	Chan	*mntpt;		/* channel in user's name space */
};

#define	BUFSIZE	(MAXFDATA+500) 	/* BUG */
typedef struct Mntbuf Mntbuf;
struct Mntbuf
{
	Mntbuf	*next;
	char	buf[BUFSIZE];
};

struct
{
	Lock;
	Mntbuf	*free;
}mntbufalloc;

typedef struct Mnthdr Mnthdr;
struct Mnthdr		/* next only meaningful when buffer isn't being used */
{
	Mnthdr	*next;
	Fcall	thdr;
	Fcall	rhdr;
};

struct
{
	Lock;
	Mnthdr	*free;
}mnthdralloc;

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
		unlock(&mnthdralloc);
		return mh;
	}
	unlock(&mnthdralloc);
	print("no mnthdrs\n");
	if(u == 0)
		panic("mballoc");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
mhfree(Mnthdr *mh)
{
	lock(&mnthdralloc);
	mh->next = mnthdralloc.free;
	mnthdralloc.free = mh;
	unlock(&mnthdralloc);
}

Mnt*
mntdev(int dev, int noerr)
{
	Mnt *m;
	int i;

	for(m=mnt,i=0; i<conf.nmntdev; i++,m++)		/* use a hash table some day */
		if(m->mntid == dev){
			if(m->msg == 0)
				break;
			return m;
		}
	if(noerr)
		return 0;
	error(0, Eshutdown);
}

void
mntreset(void)
{
	int i;
	Mntbuf *mb;
	Mnthdr *mh;

	mnt = ialloc(conf.nmntdev*sizeof(Mnt), 0);

	mb = ialloc(conf.nmntbuf*sizeof(Mntbuf), 0);
	for(i=0; i<conf.nmntbuf-1; i++)
		mb[i].next = &mb[i+1];
	mb[i].next = 0;
	mntbufalloc.free = mb;

	mh = ialloc(conf.nmnthdr*sizeof(Mnthdr), 0);
	for(i=0; i<conf.nmnthdr-1; i++)
		mh[i].next = &mh[i+1];
	mh[i].next = 0;
	mnthdralloc.free = mh;
}

void
mntinit(void)
{
}

Chan*
mntattach(char *spec)
{
	int i;
	Mnt *m;
	Mnthdr *mh;
	Chan *c, *cm;
	struct bogus{
		Chan	*chan;
		char	*spec;
	}bogus;

	bogus = *((struct bogus *)spec);
	spec = bogus.spec;

	m = mnt;
	for(i=0; i<conf.nmntdev; i++,m++){
		lock(m);
		if(m->ref == 0)
			goto Found;
		unlock(m);
	}
	error(0, Enomntdev);
    Found:
	m->ref = 1;
	unlock(m);
	lock(&mntid);
	m->mntid = ++mntid.id;
	unlock(&mntid);
	c = devattach('M', spec);
	c->dev = m->mntid;
	m->mntpt = c;
	cm = bogus.chan;
	m->msg = cm;
	incref(cm);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		close(c);
		nexterror();
	}
	mh->thdr.type = Tattach;
	mh->thdr.fid = c->fid;
	memcpy(mh->thdr.uname, u->p->pgrp->user, NAMELEN);
	strcpy(mh->thdr.aname, spec);
	mntxmit(m, mh);
	c->qid = mh->rhdr.qid;
	c->mchan = m->msg;
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
	m = mntdev(c->dev, 0);
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
	nc->mchan = c->mchan;
	nc->mqid = c->qid;
	if(new)
		poperror();
	mhfree(mh);
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
	m = mntdev(c->dev, 0);
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

	m = mntdev(c->dev, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tstat;
	mh->thdr.fid = c->fid;
	mntxmit(m, mh);
	memcpy(dp, mh->rhdr.stat, DIRLEN);
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

	m = mntdev(c->dev, 0);
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

	m = mntdev(c->dev, 0);
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
mntclose(Chan *c)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c->dev, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tclunk;
	mh->thdr.fid = c->fid;
	mntxmit(m, mh);
	mhfree(mh);
	if(c == m->mntpt)
		m->mntpt = 0;
	if(decref(m) == 0){		/* BUG: need to hang up all pending i/o */
		qlock(m);
		close(m->msg);
		m->msg = 0;
		qunlock(m);
	}
	poperror();
}

long
mntreadwrite(Chan *c, void *vbuf, long n, int type)
{
	Mnt *m;
	Mnthdr *mh;
	long nt, nr, count, offset;
	char *buf;

	buf = vbuf;
	count = 0;
	offset = c->offset;
	m = mntdev(c->dev, 0);
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
mntread(Chan *c, void *buf, long n)
{
	long i;
	uchar *b;

	n = mntreadwrite(c, buf, n, Tread);
	if(c->qid & CHDIR){
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
mntwrite(Chan *c, void *buf, long n)
{
	return mntreadwrite(c, buf, n, Twrite);
}

void	 
mntremove(Chan *c)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c->dev, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tremove;
	mh->thdr.fid = c->fid;
	mntxmit(m, mh);
	mhfree(mh);
	poperror();
}

void
mntwstat(Chan *c, char *dp)
{
	Mnt *m;
	Mnthdr *mh;

	m = mntdev(c->dev, 0);
	mh = mhalloc();
	if(waserror()){
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Twstat;
	mh->thdr.fid = c->fid;
	memcpy(mh->thdr.stat, dp, DIRLEN);
	mntxmit(m, mh);
	mhfree(mh);
	poperror();
}

void	 
mnterrstr(Error *e, char *buf)
{
	Mnt *m;
	Mnthdr *mh;
	char *def="mounted device shut down";

	m = mntdev(e->dev, 1);
	if(m == 0){
		strcpy(buf, def);
		return;
	}
	mh = mhalloc();
	if(waserror()){
		strcpy(buf, def);
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Terrstr;
	mh->thdr.fid = 0;
	mh->thdr.err = e->code;
	mntxmit(m, mh);
	strcpy(buf, (char*)mh->rhdr.ename);
	mhfree(mh);
	poperror();
}

void	 
mntuserstr(Error *e, char *buf)
{
	Mnt *m;
	Mnthdr *mh;
	char *def="mounted device shut down";

	m = mntdev(e->dev, 1);
	if(m == 0){
		strcpy(buf, def);
		return;
	}
	mh = mhalloc();
	if(waserror()){
		strcpy(buf, def);
		mhfree(mh);
		nexterror();
	}
	mh->thdr.type = Tuserstr;
	mh->thdr.fid = 0;
	mh->thdr.uid = e->code;
	mntxmit(m, mh);
	strcpy(buf, (char*)mh->rhdr.uname);
	mhfree(mh);
	poperror();
}

void
mntxmit(Mnt *m, Mnthdr *mh)
{
	ulong n;
	Mntbuf *mbr, *mbw;
	Chan *mntpt;

	mbr = mballoc();
	mbw = mballoc();
	if(waserror()){
		mbfree(mbr);
		mbfree(mbw);
		nexterror();
	}
	n = convS2M(&mh->thdr, mbw->buf);
	qlock(m);
	if(m->msg == 0){
		qunlock(m);
		error(0, Eshutdown);
	}
	qlock(m->msg);
	if(waserror()){
		qunlock(m);
		qunlock(m->msg);
		nexterror();
	}
	if((*devtab[m->msg->type].write)(m->msg, mbw->buf, n) != n){
		pprint("short write in mntxmit\n");
		error(0, Egreg);
	}

	/*
	 * Read response
	 */
	n = (*devtab[m->msg->type].read)(m->msg, mbr->buf, BUFSIZE);
	qunlock(m);
	qunlock(m->msg);
	poperror();

	if(convM2S(mbr->buf, &mh->rhdr, n) == 0){
		pprint("format error in mntxmit\n");
		error(0, Egreg);
	}

	/*
	 * Various checks
	 */
	if(mh->rhdr.type != mh->thdr.type+1){
		pprint("type mismatch %d %d\n", mh->rhdr.type, mh->thdr.type+1);
		error(0, Egreg);
	}
	if(mh->rhdr.fid != mh->thdr.fid){
		pprint("fid mismatch %d %d type %d\n", mh->rhdr.fid, mh->thdr.fid, mh->rhdr.type);
		error(0, Egreg);
	}
	if(mh->rhdr.err){
		mntpt = m->mntpt;	/* unsafe, but Errors are unsafe anyway */
		if(mntpt)
			error(mntpt, mh->rhdr.err);
		error(0, Eshutdown);
	}
	/*
	 * Copy out on read
	 */
	if(mh->thdr.type == Tread)
		memcpy(mh->thdr.data, mh->rhdr.data, mh->rhdr.count);
	mbfree(mbr);
	mbfree(mbw);
	poperror();
}
