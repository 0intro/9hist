#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"fcall.h"

#include	"devtab.h"

typedef struct Mux Mux;
typedef struct Con Con;
typedef struct Dtq Dtq;

enum
{
	Qdir	= 0,
	Qhead,
	Qclone,
};

enum
{
	Nmux	=	20,
};

struct Dtq
{
	QLock	rd;
	Rendez	r;
	Lock	listlk;
	Block	*list;
	int	ndelim;
};

struct Con
{
	int	ref;
	char	user[NAMELEN];
	ulong	perm;
	Dtq	conq;
};

struct Mux
{
	Ref;
	char	name[NAMELEN];
	char	user[NAMELEN];
	ulong	perm;
	int	headopen;
	Dtq	headq;
	Con	connects[Nmux];
};

Mux	*muxes;

ulong	muxreadq(Mux *m, Dtq*, char*, ulong);
void	muxwriteq(Dtq*, char*, long, int, int);

#define NMUX(c)		(((c->qid.path>>8)&0xffff)-1)
#define NQID(m, c)	(Qid){(m)<<8|(c)&0xff, 0}
#define NCON(c)		(c->qid.path&0xff)

int
muxgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Mux *m;
	int mux;
	Con *cm;
	char buf[10];

	if(c->qid.path == CHDIR) {
		if(s >= conf.nmux)
			return -1;

		m = &muxes[s];
		if(m->name[0] == '\0')
			return 0;
		devdir(c, (Qid){CHDIR|((s+1)<<8), 0}, m->name, 0, m->user, m->perm, dp);
		return 1;
	}

	if(s >= Nmux+2)
		return -1;

	mux = NMUX(c);
	m = &muxes[mux];
	switch(s) {
	case Qhead:
		devdir(c, NQID(mux, Qhead), "head", m->headq.ndelim, m->user, m->perm, dp);
		break;
	case Qclone:
		devdir(c, NQID(mux, Qclone), "clone", 0, m->user, m->perm, dp);
		break;
	default:
		cm = &m->connects[s-Qclone];
		if(cm->ref == 0)
			return 0;
		sprint(buf, "%d", s-Qclone);
		devdir(c, NQID(mux, Qclone+s), buf, cm->conq.ndelim, cm->user, cm->perm, dp);
		break;
	}
	return 1;
}

void
muxinit(void)
{
}

void
muxreset(void)
{
	muxes = ialloc(conf.nmux*sizeof(Mux), 0);
}

Chan *
muxattach(char *spec)
{
	Chan *c;

	c = devattach('m', spec);

	c->qid.path = CHDIR|Qdir;
	return c;
}

Chan *
muxclone(Chan *c, Chan *nc)
{
	int ncon;
	Mux *m;

	if(c->qid.path == CHDIR)
		return devclone(c, nc);;

	m = &muxes[NMUX(c)];
	ncon = NCON(c);

	c = devclone(c, nc);
	switch(ncon) {
	case Qhead:
		incref(m);
		break;
	case Qclone:
		break;
	default:
		lock(m);
		m->connects[ncon].ref++;
		m->ref++;
		unlock(m);
	}
	return c;
}

int
muxwalk(Chan *c, char *name)
{
	if(strcmp(name, "..") == 0) {
		c->qid.path = CHDIR|Qdir;
		return 1;
	}

	return devwalk(c, name, 0, 0, muxgen);
}

void
muxstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, muxgen);
}

Chan *
muxopen(Chan *c, int omode)
{
	Mux *m;
	Con *cm, *e;

	if(c->qid.path & CHDIR)
		return devopen(c, omode, 0, 0, muxgen);

	m = &muxes[NMUX(c)];
	switch(NCON(c)) {
	case Qhead:
		if(m->headopen)
			errors("server channel busy");

		c = devopen(c, omode, 0, 0,muxgen);
		m->headopen = 1;
		incref(m);
		break;
	case Qclone:
		if(m->headopen == 0)
			errors("server shutdown");

		c = devopen(c, omode, 0, 0, muxgen);
		lock(m);
		cm = m->connects;
		for(e = &cm[Nmux]; cm < e; cm++)
			if(cm->ref == 0)
				break;
		if(cm == e) {
			unlock(m);
			errors("all cannels busy");
		}
		cm->ref++;
		m->ref++;
		unlock(m);
		strncpy(cm->user, u->p->user, NAMELEN);
		cm->perm = 0600;
		c->qid = NQID(NMUX(c), cm-m->connects);
		break;
	default:
		c = devopen(c, omode, 0, 0,muxgen);
		cm = &m->connects[NCON(c)];
		cm->ref++;
		incref(m);
		break;
	}

	return c;
}

void
muxcreate(Chan *c, char *name, int omode, ulong perm)
{
	int n;
	Mux *m, *e;

	if(c->qid.path != CHDIR)
		error(Eperm);

	omode = openmode(omode);

	m = muxes;
	for(e = &m[conf.nmux]; m < e; m++) {
		if(m->ref == 0 && canlock(m)) {
			if(m->ref != 0) {
				unlock(m);
				continue;
			}
			m->ref++;
			break;
		}	
	}

	if(m == e)
		errors("no multiplexors");

	strncpy(m->name, name, NAMELEN);
	strncpy(m->user, u->p->user, NAMELEN);
	m->perm = perm&~CHDIR;
	unlock(m);

	n = m - muxes;
	c->qid = (Qid){CHDIR|(n+1)<<8, 0};
	c->flag |= COPEN;
	c->mode = omode;
}

void
muxremove(Chan *c)
{
	Mux *m;

	if(c->qid.path == CHDIR || (c->qid.path&CHDIR) == 0)
		error(Eperm);

	m = &muxes[NMUX(c)];
	if(strcmp(u->p->user, m->user) != 0)
		errors("not owner");

	m->name[0] = '\0';
}

void
muxwstat(Chan *c, char *db)
{
	Mux *m;
	Dir d;
	int nc;

	if(c->qid.path == CHDIR)
		error(Eperm);

	m = &muxes[NMUX(c)];
	if(strcmp(u->p->user, m->user) != 0)
		errors("not owner");

	convM2D(db, &d);
	d.mode &= 0777;
	if(c->qid.path&CHDIR) {
		strcpy(m->name, d.name);
		strcpy(m->user, d.uid);
		m->perm = d.mode;
		return;
	}
	nc = NCON(c);
	switch(nc) {
	case Qclone:
		error(Eperm);
	case Qhead:
		m->perm = d.mode;
		break;
	default:
		m->connects[nc].perm = d.mode;
		break;
	}
}

void
muxclose(Chan *c)
{
	Block *f1, *f2;
	Con *cm, *e;
	Mux *m;
	int nc;

	if(c->qid.path == CHDIR)
		return;

	m = &muxes[NMUX(c)];
	nc = NCON(c);
	f1 = 0;
	f2 = 0;
	switch(nc) {
	case Qhead:
		m->headopen = 0;
		cm = m->connects;
		for(e = &cm[Nmux]; cm < e; cm++)
			if(cm->ref)
				wakeup(&cm->conq.r);
		lock(m);
		if(--m->ref == 0) {
			f1 = m->headq.list;
			m->headq.list = 0;
		}
		unlock(m);
		break;
	case Qclone:
		panic("muxclose");
	default:
		lock(m);
		cm = &m->connects[nc];
		if(--cm->ref == 0) {
			f1 = cm->conq.list;
			cm->conq.list = 0;		
		}
		if(--m->ref == 0) {
			m->name[0] = '\0';
			f2 = m->headq.list;
			m->headq.list = 0;
		}
		unlock(m);
	}
	if(f1)
		freeb(f1);
	if(f2)
		freeb(f2);
}

long
muxread(Chan *c, void *va, long n, ulong offset)
{
	Mux *m;
	Con *cm;
	int bread;

	if(c->qid.path & CHDIR)
		return devdirread(c, va, n, 0, 0, muxgen);

	m = &muxes[NMUX(c)];
	switch(NCON(c)) {
	case Qhead:
		bread = muxreadq(m, &m->headq, va, n);
		break;
	case Qclone:
		error(Eperm);
	default:
		cm = &m->connects[NCON(c)];
		bread = muxreadq(m, &cm->conq, va, n);
		break;
	}

	return bread;
}

Con *
muxhdr(Mux *m, char *h)
{
	Con *c;

	if(h[0] != Tmux)
		error(Ebadmsg);

	c = &m->connects[h[1]];
	if(c < m->connects || c > &m->connects[Nmux])	
		error(Ebadmsg);

	if(c->ref == 0)
		return 0;

	return c;
}

long
muxwrite(Chan *c, void *va, long n, ulong offset)
{
	Mux *m;
	Con *cm;
	int muxid;
	Block *f, *bp;
	char *a, hdr[2];

	if(c->qid.path & CHDIR)
		error(Eisdir);

	m = &muxes[NMUX(c)];
	switch(NCON(c)) {
	case Qclone:
		error(Eperm);
	case Qhead:
		if(n < 2)
			error(Ebadmsg);

		a = (char*)va;
		memmove(hdr, a, sizeof(hdr));
		cm = muxhdr(m, hdr);
		if(cm == 0)
			error(Ehungup);

		muxwriteq(&cm->conq, a+sizeof(hdr), n-sizeof(hdr), 0, 0);
		break;
	default:
		if(m->headopen == 0)
			error(Ehungup);

		muxid = NCON(c);
		muxwriteq(&m->headq, va, n, 1, muxid);
		break;
	}

	return n;
}

void
muxwriteq(Dtq *q, char *va, long n, int addid, int muxid)
{
	Block *head, *tail, *bp;
	ulong l;

	head = 0;
	SET(tail);
	if(waserror()) {
		if(head)
			freeb(head);
		nexterror();
	}

	while(n) {
		bp = allocb(n);
		bp->type = M_DATA;
		l = bp->lim - bp->wptr;
		memmove(bp->wptr, va, l);	/* Interruptable thru fault */
		va += l;
		bp->wptr += l;
		n -= l;
		if(head == 0)
			head = bp;
		else
			tail->next = bp;
		tail = bp;
	}
	poperror();
	tail->flags |= S_DELIM;
	lock(&q->listlk);
	for(tail = q->list; tail->next; tail = tail->next)
		;
	tail->next = head;
	q->ndelim++;
	unlock(&q->listlk);
}

int
nodata(Dtq *q)
{
	int n;

	lock(&q->listlk);
	n = q->ndelim;
	unlock(&q->listlk);
	return n;
}

ulong
muxreadq(Mux *m, Dtq *q, char *va, ulong n)
{
	int l, nread, gotdelim;
	Block *bp;

	qlock(&q->rd);
	bp = 0;
	if(waserror()) {
		qunlock(&q->rd);
		lock(&q->listlk);
		if(bp) {
			bp->next = q->list;
			q->list = bp;
		}
		unlock(&q->listlk);
		nexterror();
	}
	while(nodata(q))
		sleep(&q->r, nodata, q);

	if(m->headopen == 0)
		errors("server shutdown");

	nread = 0;
	while(n) {
		lock(&q->listlk);
		bp = q->list;
		q->list = bp->next;
		bp->next = 0;
		unlock(&q->listlk);

		l = BLEN(bp);
		if(n < l)
			n = l;
		memmove(va, bp->rptr, l);	/* Interruptable thru fault */
		va += l;
		bp->rptr += l;
		n -= l;
		gotdelim = bp->flags&S_DELIM;
		lock(&q->listlk);
		if(bp->rptr != bp->wptr) {
			bp->next = q->list;
			q->list = bp;
		}
		else if(gotdelim)
			q->ndelim--;
		unlock(&q->listlk);
		if(bp->rptr == bp->wptr)
			freeb(bp);
		if(gotdelim)
			break;
	}
	qunlock(&q->rd);
	return nread;
}
