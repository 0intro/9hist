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

	pgrpalloc.arena = ialloc(conf.npgrp*sizeof(Pgrp), 0);
	pgrpalloc.free = pgrpalloc.arena;

	p = pgrpalloc.free;
	for(i=0; i<conf.npgrp; i++,p++){
		p->index = i;
		p->next = p+1;
		p->mtab = ialloc(conf.nmtab*sizeof(Mtab), 0);
	}
	p[-1].next = 0;

	egrpalloc.free = ialloc(conf.npgrp*sizeof(Egrp), 0);
	ee = &egrpalloc.free[conf.npgrp];
	for(e = egrpalloc.free; e < ee; e++) {
		e->next = e+1;
		e->etab = ialloc(conf.npgenv*sizeof(Envp*), 0);
	}
	e[-1].next = 0;

	fgrpalloc.free = ialloc(conf.nproc*sizeof(Fgrp), 0);
	fe = &fgrpalloc.free[conf.nproc-1];
	for(f = fgrpalloc.free; f < fe; f++)
		f->next = f+1;
	f->next = 0;

	mountalloc.free = ialloc(conf.nmount*sizeof(Mount), 0);
	em = &mountalloc.free[conf.nmount-1];
	for(m = mountalloc.free; m < em; m++)
		m->next = m+1;
	m->next = 0;
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
			p->nmtab = 0;
			memset(p->rendhash, 0, sizeof(p->rendhash));
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
	int i;
	Mtab *m;

	if(decref(p) == 0){
		qlock(&p->debug);
		p->pgrpid = -1;
		m = p->mtab;
		for(i=0; i<p->nmtab; i++,m++)
			if(m->c){
				close(m->c);
				closemount(m->mnt);
			}
		lock(&pgrpalloc);
		p->next = pgrpalloc.free;
		pgrpalloc.free = p;
		qunlock(&p->debug);
		unlock(&pgrpalloc);
	}
}

void
closeegrp(Egrp *e)
{
	Envp *ep;
	int i;

	if(decref(e) == 0) {
		ep = e->etab;
		for(i=0; i<e->nenv; i++,ep++)
			if(ep->env)
				envpgclose(ep->env);
		lock(&egrpalloc);
		e->next = egrpalloc.free;
		egrpalloc.free = e;
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
newmount(void)
{
	Mount *m;

loop:
	lock(&mountalloc);
	if(m = mountalloc.free){		/* assign = */
		mountalloc.free = m->next;
		m->ref = 1;
		m->next = 0;
		m->mountid = ++mountalloc.mountid;
		unlock(&mountalloc);
		return m;
	}
	unlock(&mountalloc);
	print("no mounts\n");
	if(u == 0)
		panic("newmount");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
closemount(Mount *m)
{
	lock(m);
	if(m->ref == 1){
		if(m->c)
			close(m->c);
		if(m->next)
			closemount(m->next);
		unlock(m);
		lock(&mountalloc);
		m->mountid = 0;
		m->next = mountalloc.free;
		mountalloc.free = m;
		unlock(&mountalloc);
		return;
	}
	m->ref--;
	unlock(m);
}

void
envcpy(Egrp *to, Egrp *from)
{
	Envp *ep;
	Env *e;
	int i;

	lock(from);
	to->nenv = from->nenv;
	ep = to->etab;
	for(i=0; i<from->nenv; i++,ep++){
		ep->chref = 0;
		e = ep->env = from->etab[i].env;
		if(e){
			lock(e);
			if(waserror()){
				unlock(e);
				unlock(from);
				ep->env = 0;
				nexterror();
			}
			/*
			 * If pgrp being forked has an open channel
			 * on this env, it may write it after the fork
			 * so make a copy now.
			 * Don't worry about other pgrps, because they
			 * will copy if they are about to write.
			 */
			if(from->etab[i].chref){
				ep->env = copyenv(e, 0);
				unlock(ep->env);
			}else
				e->pgref++;
			poperror();
			unlock(e);
		}
	}
	unlock(from);
}

void
pgrpcpy(Pgrp *to, Pgrp *from)
{
	int i;
	Mtab *m;

	lock(from);
	memmove(to->user, from->user, NAMELEN);
	memmove(to->mtab, from->mtab, from->nmtab*sizeof(Mtab));
	to->nmtab = from->nmtab;
	m = to->mtab;
	for(i=0; i<from->nmtab; i++,m++)
		if(m->c){
			incref(m->c);
			lock(m->mnt);
			m->mnt->ref++;
			unlock(m->mnt);
		}

	unlock(from);
}
