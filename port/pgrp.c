#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

struct{
	Lock;
	Pgrp	*arena;
	Pgrp	*free;
	ulong	pgrpid;
}pgrpalloc;

struct{
	Lock;
	Mount	*free;
	ulong	mountid;
}mountalloc;

void
pgrpinit(void)
{
	int i;
	Pgrp *p;
	Mount *m;

	pgrpalloc.arena = ialloc(conf.npgrp*sizeof(Pgrp), 0);
	pgrpalloc.free = pgrpalloc.arena;

	p = pgrpalloc.free;
	for(i=0; i<conf.npgrp; i++,p++){
		p->index = i;
		p->next = p+1;
		p->mtab = ialloc(conf.nmtab*sizeof(Mtab), 0);
		p->etab = ialloc(conf.npgenv*sizeof(Envp*), 0);
	}
	p[-1].next = 0;

	mountalloc.free = ialloc(conf.nmount*sizeof(Mount), 0);

	m = mountalloc.free;
	for(i=0; i<conf.nmount-1; i++,m++)
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
	memcpy(buf, a, n);
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

loop:
	lock(&pgrpalloc);
	if(p = pgrpalloc.free){		/* assign = */
		pgrpalloc.free = p->next;
		p->ref = 1;
		p->pgrpid = ++pgrpalloc.pgrpid;
		p->nmtab = 0;
		p->nenv = 0;
		unlock(&pgrpalloc);
		return p;
	}
	unlock(&pgrpalloc);
	print("no pgrps\n");
	if(u == 0)
		panic("newpgrp");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
closepgrp(Pgrp *p)
{
	int i;
	Mtab *m;
	Envp *ep;

	if(decref(p) == 0){
		lock(&p->debug);
		p->pgrpid = -1;
		m = p->mtab;
		for(i=0; i<p->nmtab; i++,m++)
			if(m->c){
				close(m->c);
				closemount(m->mnt);
			}
		ep = p->etab;
		for(i=0; i<p->nenv; i++,ep++)
			if(ep->env)
				envpgclose(ep->env);
		lock(&pgrpalloc);
		p->next = pgrpalloc.free;
		pgrpalloc.free = p;
		unlock(&p->debug);
		unlock(&pgrpalloc);
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
pgrpcpy(Pgrp *to, Pgrp *from)
{
	int i;
	Mtab *m;
	Envp *ep;
	Env *e;

	lock(from);
	memcpy(to->user, from->user, NAMELEN);
	memcpy(to->mtab, from->mtab, from->nmtab*sizeof(Mtab));
	to->nmtab = from->nmtab;
	m = to->mtab;
	for(i=0; i<from->nmtab; i++,m++)
		if(m->c){
			incref(m->c);
			lock(m->mnt);
			m->mnt->ref++;
			unlock(m->mnt);
		}

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
