#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

static Ref pgrpid;
static Ref mountid;

void
pgrpnote(ulong noteid, char *a, long n, int flag)
{
	Proc *p, *ep;
	char buf[ERRLEN];

	if(n >= ERRLEN-1)
		error(Etoobig);

	memmove(buf, a, n);
	buf[n] = 0;
	p = proctab(0);
	ep = p+conf.nproc;
	for(; p < ep; p++) {
		if(p->state == Dead)
			continue;
		if(up != p && p->noteid == noteid && p->kp == 0) {
			qlock(&p->debug);
			if(p->pid == 0 || p->noteid != noteid){
				qunlock(&p->debug);
				continue;
			}
			if(!waserror()) {
				postnote(p, 0, buf, flag);
				poperror();
			}
			qunlock(&p->debug);
		}
	}
}

Pgrp*
newpgrp(void)
{
	Pgrp *p;

	p = smalloc(sizeof(Pgrp));
	p->ref = 1;
	p->pgrpid = incref(&pgrpid);
	return p;
}

Rgrp*
newrgrp(void)
{
	Rgrp *r;

	r = smalloc(sizeof(Rgrp));
	r->ref = 1;
	return r;
}

void
closergrp(Rgrp *r)
{
	if(decref(r) == 0)
		free(r);
}

void
closepgrp(Pgrp *p)
{
	Mhead **h, **e, *f, *next;
	
	if(decref(p) == 0){
		qlock(&p->debug);
		p->pgrpid = -1;

		e = &p->mnthash[MNTHASH];
		for(h = p->mnthash; h < e; h++) {
			for(f = *h; f; f = next) {
				close(f->from);
				mountfree(f->mount);
				next = f->hash;
				free(f);
			}
		}
		qunlock(&p->debug);
		free(p);
	}
}

void
pgrpinsert(Mount **order, Mount *m)
{
	Mount *f;

	m->order = 0;
	if(*order == 0) {
		*order = m;
		return;
	}
	for(f = *order; f; f = f->order) {
		if(m->mountid < f->mountid) {
			m->order = f;
			*order = m;
			return;
		}
		order = &f->order;
	}
	*order = m;
}

/*
 * pgrpcpy MUST preserve the mountid allocation order of the parent group
 */
void
pgrpcpy(Pgrp *to, Pgrp *from)
{
	int i;
	Mount *n, *m, **link, *order;
	Mhead *f, **tom, **l, *mh;

	rlock(&from->ns);
	order = 0;
	tom = to->mnthash;
	for(i = 0; i < MNTHASH; i++) {
		l = tom++;
		for(f = from->mnthash[i]; f; f = f->hash) {
			mh = smalloc(sizeof(Mhead));
			mh->from = f->from;
			incref(mh->from);
			*l = mh;
			l = &mh->hash;
			link = &mh->mount;
			for(m = f->mount; m; m = m->next) {
				n = smalloc(sizeof(Mount));
				n->to = m->to;
				incref(n->to);
				n->head = mh;
				n->flag = m->flag;
				if(m->spec != 0)
					strcpy(n->spec, m->spec);
				m->copy = n;
				pgrpinsert(&order, m);
				*link = n;
				link = &n->next;	
			}
		}
	}
	/*
	 * Allocate mount ids in the same sequence as the parent group
	 */
	lock(&mountid);
	for(m = order; m; m = m->order)
		m->copy->mountid = mountid.ref++;
	unlock(&mountid);
	runlock(&from->ns);
}

Fgrp*
dupfgrp(Fgrp *f)
{
	Fgrp *new;
	Chan *c;
	int i;

	new = smalloc(sizeof(Fgrp));
	new->ref = 1;

	lock(f);
	new->maxfd = f->maxfd;
	for(i = 0; i <= f->maxfd; i++) {
		if(c = f->fd[i]){
			incref(c);
			new->fd[i] = c;
		}
	}
	unlock(f);

	return new;
}

void
closefgrp(Fgrp *f)
{
	int i;
	Chan *c;

	if(decref(f) == 0) {
		for(i = 0; i <= f->maxfd; i++)
			if(c = f->fd[i])
				close(c);

		free(f);
	}
}

Mount*
newmount(Mhead *mh, Chan *to, int flag, char *spec)
{
	Mount *m;

	m = smalloc(sizeof(Mount));
	m->to = to;
	m->head = mh;
	incref(to);
	m->mountid = incref(&mountid);
	m->flag = flag;
	if(spec != 0)
		strcpy(m->spec, spec);

	return m;
}

void
mountfree(Mount *m)
{
	Mount *f;

	while(m) {
		f = m->next;
		close(m->to);
		m->mountid = 0;
		free(m);
		m = f;
	}
}

void
resrcwait(char *reason)
{
	char *p;

	if(up == 0)
		panic("resrcwait");

	p = up->psstate;
	if(reason) {
		up->psstate = reason;
		print("%s\n", reason);
	}

	tsleep(&up->sleep, return0, 0, 300);
	up->psstate = p;
}
