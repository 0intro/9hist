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
		if(p->noteid == noteid && p->kp == 0) {
			qlock(&p->debug);
			if(p->pid==0 || p->noteid != noteid){
				qunlock(&p->debug);
				continue;
			}
			if(!waserror()){
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

	p = smalloc(sizeof(Pgrp)+sizeof(Crypt));
	p->ref = 1;
	/* This needs to have its own arena for protection */
	p->crypt = (Crypt*)((uchar*)p+sizeof(Pgrp));
	p->pgrpid = incref(&pgrpid);
	return p;
}

Egrp*
newegrp(void)
{
	Egrp *e;

	e = smalloc(sizeof(Egrp)+sizeof(Env)*conf.npgenv);

	/* This is a sleazy hack to make malloc work .. devenv need rewriting. */
	e->etab = (Env*)((uchar*)e+sizeof(Egrp));
	e->ref = 1;
	return e;
}

Fgrp*
newfgrp(void)
{
	Fgrp *f;

	f = smalloc(sizeof(Fgrp));
	f->ref = 1;
	return f;
}

Fgrp*
dupfgrp(Fgrp *f)
{
	Fgrp *new;
	Chan *c;
	int i;

	new = newfgrp();

	lock(f);
	new->maxfd = f->maxfd;
	for(i = 0; i <= f->maxfd; i++)
		if(c = f->fd[i]){
			incref(c);
			new->fd[i] = c;
		}
	unlock(f);

	return new;
}

void
resrcwait(char *reason)
{
	char *p;

	p = u->p->psstate;
	if(reason) {
		u->p->psstate = reason;
		print("%s\n", reason);
	}
	if(u == 0)
		panic("resrcwait");

	tsleep(&u->p->sleep, return0, 0, 1000);
	u->p->psstate = p;
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
closeegrp(Egrp *eg)
{
	Env *e;
	int i;

	if(decref(eg) == 0) {
		e = eg->etab;
		for(i=0; i<eg->nenv; i++, e++)
			envpgclose(e);

		free(eg);
	}
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
newmount(Mhead *mh, Chan *to)
{
	Mount *m, *f, *e;

	m = smalloc(sizeof(Mount));
	m->to = to;
	m->head = mh;
	incref(to);
	m->mountid = incref(&mountid);
	return m;
}

void
envcpy(Egrp *to, Egrp *from)
{
	Env *te, *fe;
	int i, nenv;

	qlock(&from->ev);
	nenv = from->nenv;
	to->nenv = nenv;
	te = to->etab;
	fe = from->etab;
	for(i=0; i < nenv; i++, te++, fe++)
		envpgcopy(te, fe);
	qunlock(&from->ev);
}

void
pgrpcpy(Pgrp *to, Pgrp *from)
{
	Mhead **h, **e, *f, **tom, **l, *mh;
	Mount *n, *m, **link;

	rlock(&from->ns);

	*to->crypt = *from->crypt;
	e = &from->mnthash[MNTHASH];
	tom = to->mnthash;
	for(h = from->mnthash; h < e; h++) {
		l = tom++;
		for(f = *h; f; f = f->hash) {
			mh = smalloc(sizeof(Mhead));
			mh->from = f->from;
			incref(mh->from);
			*l = mh;
			l = &mh->hash;
			link = &mh->mount;
			for(m = f->mount; m; m = m->next) {
				n = newmount(mh, m->to);
				*link = n;
				link = &n->next;	
			}
		}
	}
	runlock(&from->ns);
}

void
mountfree(Mount *m)
{
	Mount *f;

	for(f = m; f->next; f = f->next)
		close(f->to);

	close(f->to);

	free(f);
}
