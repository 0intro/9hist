#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"devtab.h"
#include	"netif.h"

typedef struct Pipe	Pipe;
struct Pipe
{
	QLock;
	Pipe	*next;
	int	ref;
	ulong	path;
	Queue	*q[2];
	int	qref[2];
};

struct
{
	Lock;
	Pipe	*pipe;
	ulong	path;
} pipealloc;

enum
{
	Qdir,
	Qdata0,
	Qdata1,
};

Dirtab pipedir[] =
{
	"data",		{Qdata0},	0,			0600,
	"data1",	{Qdata1},	0,			0600,
};
#define NPIPEDIR 4

void
pipeinit(void)
{
}

void
pipereset(void)
{
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
	p = malloc(sizeof(Pipe));
	if(p == 0)
		exhausted("memory");
	p->ref = 1;

	p->q[0] = qopen(64*1024, 0, 0, 0);
	if(p->q[0] == 0){
		free(p);
		exhausted("memory");
	}
	p->q[1] = qopen(32*1024, 0, 0, 0);
	if(p->q[1] == 0){
		free(p->q[0]);
		free(p);
		exhausted("memory");
	}

	lock(&pipealloc);
	p->path = ++pipealloc.path;
	p->next = pipealloc.pipe;
	pipealloc.pipe = p;
	unlock(&pipealloc);

	c->qid = (Qid){CHDIR|NETQID(2*p->path, Qdir), 0};
	c->aux = p;
	c->dev = 0;
	return c;
}

Chan*
pipeclone(Chan *c, Chan *nc)
{
	Pipe *p;

	p = c->aux;
	nc = devclone(c, nc);
	qlock(p);
	p->ref++;
	qunlock(p);
	return nc;
}

int
pipegen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	int id;

	id = NETID(c->qid.path);
	if(i > 1)
		id++;
	if(tab==0 || i>=ntab)
		return -1;
	tab += i;
	devdir(c, (Qid){NETQID(id, tab->qid.path),0}, tab->name, tab->length, eve, tab->perm, dp);
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
	Pipe *p;
	Dir dir;

	p = c->aux;

	switch(NETTYPE(c->qid.path)){
	case Qdir:
		devdir(c, c->qid, ".", 2*DIRLEN, eve, CHDIR|0555, &dir);
		break;
	case Qdata0:
		devdir(c, c->qid, "data", qlen(p->q[0]), eve, 0660, &dir);
		break;
	case Qdata1:
		devdir(c, c->qid, "data1", qlen(p->q[1]), eve, 0660, &dir);
		break;
	default:
		panic("pipestat");
	}
	convD2M(&dir, db);
}

/*
 *  if the stream doesn't exist, create it
 */
Chan *
pipeopen(Chan *c, int omode)
{
	Pipe *p;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	p = c->aux;
	qlock(p);
	switch(NETTYPE(c->qid.path)){
	case Qdata0:
		p->qref[0]++;
		break;
	case Qdata1:
		p->qref[1]++;
		break;
	}
	qunlock(p);

	c->mode = omode&~OTRUNC;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
pipecreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Egreg);
}

void
piperemove(Chan *c)
{
	USED(c);
	error(Egreg);
}

void
pipewstat(Chan *c, char *db)
{
	USED(c, db);
	error(Eperm);
}

void
pipeclose(Chan *c)
{
	Pipe *p, *f, **l;

	p = c->aux;
	qlock(p);

	/*
	 *  closing either side hangs up the stream
	 */
	switch(NETTYPE(c->qid.path)){
	case Qdata0:
		p->qref[0]--;
		if(p->qref[0] == 0){
			qclose(p->q[0]);
			qhangup(p->q[1]);
		}
		break;
	case Qdata1:
		p->qref[1]--;
		if(p->qref[1] == 0){
			qclose(p->q[1]);
			qhangup(p->q[0]);
		}
		break;
	}

	/*
	 *  if both sides are closed, they are reusable
	 */
	if(p->qref[0] == 0 && p->qref[1] == 0){
		qreopen(p->q[0]);
		qreopen(p->q[1]);
	}

	/*
	 *  free the structure on last close
	 */
	p->ref--;
	if(p->ref == 0){
		qunlock(p);
		lock(&pipealloc);
		l = &pipealloc.pipe;
		for(f = *l; f; f = f->next) {
			if(f == p) {
				*l = p->next;
				break;
			}
			l = &f->next;
		}
		unlock(&pipealloc);
		free(p->q[0]);
		free(p->q[1]);
		free(p);
	}

	qunlock(p);
}

long
piperead(Chan *c, void *va, long n, ulong offset)
{
	Pipe *p;

	USED(offset);

	p = c->aux;

	switch(NETTYPE(c->qid.path)){
	case Qdir:
		return devdirread(c, va, n, pipedir, NPIPEDIR, pipegen);
	case Qdata0:
		return qread(p->q[0], va, n);
	case Qdata1:
		return qread(p->q[1], va, n);
	default:
		panic("piperead");
	}
	return -1;	/* not reached */
}

/*
 *  a write to a closed pipe causes a note to be sent to
 *  the process.
 */
long
pipewrite(Chan *c, void *va, long n, ulong offset)
{
	Pipe *p;

	USED(offset);

	p = c->aux;

	switch(NETTYPE(c->qid.path)){
	case Qdata0:
		return qwrite(p->q[1], va, n, 0);
	case Qdata1:
		return qwrite(p->q[0], va, n, 0);
	default:
		panic("piperead");
	}
	return n;
}
