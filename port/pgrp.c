#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

struct
{
	Lock;
	Pgrp	*arena;
	Pgrp	*free;
	ulong	pgrpid;
}pgrpalloc;

struct
{
	Lock;
	Egrp	*free;
}egrpalloc;

struct
{
	Lock;
	Fgrp	*free;
}fgrpalloc;

struct{
	Lock;
	Mount	*free;
	Mhead	*mhfree;
	ulong	mountid;
}mountalloc;

void
grpinit(void)
{
	int i;
	Pgrp *p;
	Egrp *e, *ee;
	Fgrp *f, *fe;
	Mount *m, *em;
	Mhead *hm, *hem;

	pgrpalloc.arena = ialloc(conf.npgrp*sizeof(Pgrp), 0);
	pgrpalloc.free = pgrpalloc.arena;

	p = pgrpalloc.free;
	for(i=0; i<conf.npgrp; i++,p++) {
		p->index = i;
		p->next = p+1;
	}
	p[-1].next = 0;

	egrpalloc.free = ialloc(conf.npgrp*sizeof(Egrp), 0);
	ee = &egrpalloc.free[conf.npgrp];
	for(e = egrpalloc.free; e < ee; e++) {
		e->next = e+1;
		e->etab = ialloc(conf.npgenv*sizeof(Env), 0);
	}
	e[-1].next = 0;

	fgrpalloc.free = ialloc(conf.nproc*sizeof(Fgrp), 0);
	fe = &fgrpalloc.free[conf.nproc-1];
	for(f = fgrpalloc.free; f < fe; f++)
		f->next = f+1;
	f->next = 0;
}

Pgrp*
pgrptab(int i)
{
	return &pgrpalloc.arena[i];
}

void
pgrpnote(Pgrp *pg, char *a, long n, int flag)
{
	int i;
	Proc *p;
	char buf[ERRLEN];

	if(n >= ERRLEN-1)
		error(Etoobig);
	if(n>=4 && strncmp(a, "sys:", 4)==0)
		error(Ebadarg);
	memmove(buf, a, n);
	buf[n] = 0;
	p = proctab(0);
	for(i=0; i<conf.nproc; i++, p++){
		if(p->pgrp == pg){
			lock(&p->debug);
			if(p->pid==0 || p->pgrp!=pg){
				unlock(&p->debug);
				continue;
			}
			if(!waserror()){
				postnote(p, 0, buf, flag);
				poperror();
			}
			unlock(&p->debug);
		}
	}
}

Pgrp*
newpgrp(void)
{
	Pgrp *p;

	for(;;) {
		lock(&pgrpalloc);
		if(p = pgrpalloc.free){
			pgrpalloc.free = p->next;
			p->ref = 1;
			p->pgrpid = ++pgrpalloc.pgrpid;
			memset(p->rendhash, 0, sizeof(p->rendhash));
			memset(p->mnthash, 0, sizeof(p->mnthash));
			unlock(&pgrpalloc);
			return p;
		}
		unlock(&pgrpalloc);
		resrcwait("no pgrps");
	}
}

Egrp*
newegrp(void)
{
	Egrp *e;

	for(;;) {
		lock(&egrpalloc);
		if(e = egrpalloc.free) {
			egrpalloc.free = e->next;
			e->ref = 1;
			e->nenv = 0;
			unlock(&egrpalloc);
			return e;
		}
		unlock(&egrpalloc);
		resrcwait("no envgrps");
	}
}

Fgrp*
newfgrp(void)
{
	Fgrp *f;

	for(;;) {
		lock(&fgrpalloc);
		if(f = fgrpalloc.free) {
			fgrpalloc.free = f->next;
			f->ref = 1;
			f->maxfd = 0;
			memset(f->fd, 0, sizeof(f->fd));
			unlock(&fgrpalloc);
			return f;
		}
		unlock(&fgrpalloc);
		resrcwait("no filegrps");
	}
}

Fgrp*
dupfgrp(Fgrp *f)
{
	Fgrp *new;
	Chan *c;
	int i;

	new = newfgrp();

	new->maxfd = f->maxfd;
	for(i = 0; i <= f->maxfd; i++)
		if(c = f->fd[i]){
			incref(c);
			new->fd[i] = c;
		}

	return new;
}

void
resrcwait(char *reason)
{
	if(reason)
		print("%s\n", reason);
	if(u == 0)
		panic("resrcwait");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
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
				mntheadfree(f);
			}
		}

		lock(&pgrpalloc);
		p->next = pgrpalloc.free;
		pgrpalloc.free = p;
		qunlock(&p->debug);
		unlock(&pgrpalloc);
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
		lock(&egrpalloc);
		eg->next = egrpalloc.free;
		egrpalloc.free = eg;
		unlock(&egrpalloc);
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

		lock(&fgrpalloc);
		f->next = fgrpalloc.free;
		fgrpalloc.free = f;
		unlock(&fgrpalloc);
	}
}


Mount*
newmount(Mhead *mh, Chan *to)
{
	Mount *m, *f, *e;

	for(;;) {
		lock(&mountalloc);
		if(m = mountalloc.free){		/* assign = */
			mountalloc.free = m->next;
			m->mountid = ++mountalloc.mountid;
			unlock(&mountalloc);
			m->next = 0;
			m->head = mh;
			m->to = to;
			incref(to);
			return m;
		}
		unlock(&mountalloc);

		m = (Mount*)VA(kmap(newpage(0, 0, 0)));
		e = &m[(BY2PG/sizeof(Mount))-1];
		for(f = m; f < e; f++)
			f->next = f+1;

		lock(&mountalloc);
		e->next = mountalloc.free;
		mountalloc.free = m;
		unlock(&mountalloc);
	}
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
	Mhead **h, **e, *f, **l, *mh;
	Mount *n, *m, **link;

	memmove(to->user, from->user, NAMELEN);

	rlock(&from->ns);

	e = &from->mnthash[MNTHASH];
	for(h = from->mnthash; h < e; h++) {
		for(f = *h; f; f = f->hash) {
			mh = newmnthead();
			mh->from = f->from;
			incref(mh->from);
			l = &MOUNTH(to, mh->from);
			mh->hash = *l;
			*l = mh;
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

Mhead *
newmnthead(void)
{
	Mhead *mh, *f, *e;

	for(;;) {
		lock(&mountalloc);
		if(mh = mountalloc.mhfree) {		/* Assign '=' */
			mountalloc.mhfree = mh->hash;
			unlock(&mountalloc);
			mh->hash = 0;
			mh->mount = 0;
			return mh;
		}
		unlock(&mountalloc);

		mh = (Mhead*)VA(kmap(newpage(0, 0, 0)));
		e = &mh[(BY2PG/sizeof(Mhead))-1];
		for(f = mh; f < e; f++)
			f->hash = f+1;

		lock(&mountalloc);
		e->hash = mountalloc.mhfree;
		mountalloc.mhfree = mh;
		unlock(&mountalloc);
	}
}

void
mntheadfree(Mhead *mh)
{
	lock(&mountalloc);
	mh->hash = mountalloc.mhfree;
	mountalloc.mhfree = mh;
	unlock(&mountalloc);
}

void
mountfree(Mount *m)
{
	Mount *f;

	for(f = m; f->next; f = f->next)
		close(f->to);
	close(f->to);
	lock(&mountalloc);
	f->next = mountalloc.free;
	mountalloc.free = m;
	unlock(&mountalloc);
}
