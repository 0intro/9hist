#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

/*
 * An environment value is kept in some number of contiguous
 * Envvals, with the Env's val pointing at the first.
 * Envvals are allocated from the end of a fixed arena, which
 * is compacted when the arena end is reached.
 * A `piece' (number of contiguous Envvals) is free to be
 * reclaimed if its e pointer is 0.
 *
 * Locking: an env's val can change by compaction, so lock
 * an env before using its value.  A pgrp env[] slot can go
 * to 0 and the corresponding env freed (by envremove()), so
 * lock the pgrp around the use of a value retrieved from a slot.
 * Lock in order: pgrp, envalloc, env (but ok to skip envalloc
 * lock if there is no possibility of blocking).
 */

struct Envval
{
	ulong	n;	/* number of Envval's (including this) in this piece */
	ulong	len;	/* how much of dat[] is valid */
	Env	*e;	/* the Env whose val points here */
	char	dat[4]; /* possibly extends into further envvals after this */
};

/* number of contiguous Envvals needed to hold n characters */
#define EVNEEDED(n) ((n)<4? 1 : 1+((n)+(sizeof(Envval))-1-4)/(sizeof(Envval)))

struct
{
	Lock;
	Envval	*arena;
	Envval	*vfree;
	Envval	*end;
	Env	*efree;
	Env	*earena;
}envalloc;

void	compactenv(Env *, ulong);

void
envreset(void)
{
	int i, n;

	n = EVNEEDED(conf.nenvchar);
	envalloc.arena = ialloc(n*sizeof(Envval), 0);
	envalloc.vfree = envalloc.arena;
	envalloc.end = envalloc.arena+n;

	envalloc.earena = ialloc(conf.nenv*sizeof(Env), 0);
	envalloc.efree = envalloc.earena;
	for(i=0; i<conf.nenv-1; i++)
		envalloc.earena[i].next = &envalloc.earena[i+1];
	envalloc.earena[conf.nenv-1].next = 0;
}

void
envinit(void)
{
}

/*
 * Make sure e->val points at a value big enough to hold nchars chars.
 * The caller should fix e->val->len.
 * envalloc and e should be locked
 */
void
growenval(Env *e, ulong nchars)
{
	Envval *p;
	ulong n, nfree;

	n = EVNEEDED(nchars);
	if(p = e->val){		/* assign = */
		if(p->n < n){
			if(p+p->n == envalloc.vfree){
				compactenv(e, n - p->n);
				p = e->val;
				envalloc.vfree += n - p->n;
			}else{
				compactenv(e, n);
				p = envalloc.vfree;
				envalloc.vfree += n;
				memmove(p, e->val, e->val->n*sizeof(Envval));
				p->e = e;
				e->val->e = 0;
				e->val = p;
			}
			p->n = n;
		}
	}else{
		compactenv(e, n);
		p = envalloc.vfree;
		envalloc.vfree += n;
		p->n = n;
		p->e = e;
		e->val = p;
	}
}

/*
 * Make sure there is room for n Envval's at the end of envalloc.vfree.
 * Call this with envalloc and e locked.
 */
void
compactenv(Env *e, ulong n)
{
	Envval *p1, *p2;
	Env *p2e;

	if(envalloc.end-envalloc.vfree >= n)
		return;
	p1 = envalloc.arena;	/* dest */
	p2 = envalloc.arena;	/* source */
	while(p2 < envalloc.vfree){
		p2e = p2->e;
		if(p2e == 0){
    Free:
			p2 += p2->n;
			continue;
		}
		if(p2e<envalloc.earena || p2e>=envalloc.earena+conf.nenv){
			print("%lux not an env\n", p2e);
			panic("compactenv");
		}
		if(p1 != p2){
			if(p2e != e)
				lock(p2e);
			if(p2->e != p2e){	/* freed very recently */
				print("compactenv p2e moved\n");
				if(p2->e)
					panic("compactenv p2->e %lux\n", p2->e);
				unlock(p2e);
				goto Free;
			}
			if(p2+p2->n > envalloc.end)
				panic("compactenv copying too much");
			memmove(p1, p2, p2->n*sizeof(Envval));
			p2e->val = p1;
			if(p2e != e)
				unlock(p2e);
		}
		p2 += p1->n;
		p1 += p1->n;
	}
	envalloc.vfree = p1;
	if(envalloc.end-envalloc.vfree < n){
		print("env compact failed\n");
		error(Enoenv);
	}
}

/*
 * Return an env with a copy of e's value.
 * envalloc and e should be locked,
 * and the value returned will be locked too.
 */
Env *
copyenv(Env *e, int trunc)
{
	Env *ne;
	int n;

	ne = envalloc.efree;
	if(!ne){
		print("out of envs\n");
		error(Enoenv);
	}
	envalloc.efree = ne->next;
	lock(ne);
	if(waserror()){
		unlock(ne);
		nexterror();
	}
	ne->next = 0;
	ne->pgref = 1;
	strncpy(ne->name, e->name, NAMELEN);
	if(e->val && !trunc){
		n = e->val->len;
		/*
		 * growenval can't hold the lock on another env
		 * because compactenv assumes only one is held
		 */
		unlock(e);
		growenval(ne, n);
		lock(e);
		if(n != e->val->len){
			print("e changed in copyenv\n");
			if(n > ne->val->len)
				n = ne->val->len;
		}
		if((char*)(ne->val+ne->val->n) < ne->val->dat+n)
			panic("copyenv corrupt");
		memmove(ne->val->dat, e->val->dat, n);
		ne->val->len = n;
	}
	poperror();
	return ne;
}

int
envgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Env *e;
	Egrp *eg;
	int ans;

	eg = u->p->egrp;
	lock(eg);
	if(s >= eg->nenv)
		ans = -1;
	else{
		e = eg->etab[s].env;
		if(e == 0)
			ans = 0;
		else{
			lock(e);
			devdir(c, (Qid){s+1,0}, e->name, e->val? e->val->len : 0, 0666, dp);
			unlock(e);
			ans = 1;
		}
	}
	unlock(eg);
	return ans;
}

Chan*
envattach(char *spec)
{
	return devattach('e', spec);
}

Chan*
envclone(Chan *c, Chan *nc)
{
	Egrp *eg;

	if(!(c->qid.path&CHDIR)){
		eg = u->p->egrp;
		lock(eg);
		eg->etab[c->qid.path-1].chref++;
		unlock(eg);
	}
	return devclone(c, nc);
}

int
envwalk(Chan *c, char *name)
{
	Egrp *eg;

	if(devwalk(c, name, 0, 0, envgen)){
		if(!(c->qid.path&CHDIR)){
			eg = u->p->egrp;
			lock(eg);
			eg->etab[c->qid.path-1].chref++;
			unlock(eg);
			return 1;
		}
	}
	return 0;
}

void
envstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, envgen);
}

Chan *
envopen(Chan *c, int omode)
{
	Env *e, *ne;
	Envp *ep;
	Egrp *eg;

	if(omode & (OWRITE|OTRUNC)){
		if(c->qid.path & CHDIR)
			error(Eperm);
		eg = u->p->egrp;
		lock(eg);
		ep = &eg->etab[c->qid.path-1];
		e = ep->env;
		if(!e){
			unlock(eg);
			error(Egreg);
		}
		lock(&envalloc);
		lock(e);
		if(waserror()){
			unlock(e);
			unlock(&envalloc);
			unlock(eg);
			nexterror();
		}
		if(e->pgref == 0)
			panic("envopen");
		if(e->pgref == 1){
			if((omode&OTRUNC) && e->val){
				e->val->e = 0;
				e->val = 0;
			}
		}else{
			ne = copyenv(e, omode&OTRUNC);
			e->pgref--; /* it will still be positive */
			ep->env = ne;
			unlock(ne);
		}
		poperror();
		unlock(e);
		unlock(&envalloc);
		unlock(eg);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
envcreate(Chan *c, char *name, int omode, ulong perm)
{
	Env *e;
	Egrp *eg;
	int i;

	if(c->qid.path != CHDIR)
		error(Eperm);
	eg = u->p->egrp;
	lock(eg);
	lock(&envalloc);
	if(waserror()){
		unlock(&envalloc);
		unlock(eg);
		nexterror();
	}
	e = envalloc.efree;
	if(e == 0){
		print("out of envs\n");
		error(Enoenv);
	}
	envalloc.efree = e->next;
	e->next = 0;
	e->pgref = 1;
	strncpy(e->name, name, NAMELEN);
	if(eg->nenv == conf.npgenv){
		for(i = 0; i<eg->nenv; i++)
			if(eg->etab[i].chref == 0)
				break;
		if(i == eg->nenv){
			print("out of egroup envs\n");
			error(Enoenv);
		}
	}else
		i = eg->nenv++;
	c->qid.path = i+1;
	eg->etab[i].env = e;
	eg->etab[i].chref = 1;
	unlock(&envalloc);
	unlock(eg);
	c->offset = 0;
	c->mode = openmode(omode);
	poperror();
	c->flag |= COPEN;
}

void
envremove(Chan *c)
{
	Env *e;
	Envp *ep;
	Egrp *eg;

	if(c->qid.path & CHDIR)
		error(Eperm);
	eg = u->p->egrp;
	lock(eg);
	ep = &eg->etab[c->qid.path-1];
	e = ep->env;
	if(!e){
		unlock(eg);
		error(Enonexist);
	}
	ep->env = 0;
	ep->chref--;
	envpgclose(e);
	unlock(eg);
}

void
envwstat(Chan *c, char *db)
{	int dumpenv(void);
	dumpenv();  /*DEBUG*/
	print("envwstat\n");
	error(Egreg);
}

void
envclose(Chan * c)
{
	Egrp *eg;

	if(c->qid.path & CHDIR)
		return;
	eg = u->p->egrp;
	lock(eg);
	eg->etab[c->qid.path-1].chref--;
	unlock(eg);
}

void
envpgclose(Env *e)
{
	lock(&envalloc);
	lock(e);
	if(--e->pgref <= 0){
		if(e->val){
			e->val->e = 0;
			e->val = 0;
		}
		e->next = envalloc.efree;
		envalloc.efree = e;
	}
	unlock(e);
	unlock(&envalloc);
}

long
envread(Chan *c, void *va, long n, ulong offset)
{
	Env *e;
	Envval *ev;
	char *p;
	long vn;
	Egrp *eg;
	char *a = va;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, envgen);
	eg = u->p->egrp;
	lock(eg);
	e = eg->etab[c->qid.path-1].env;
	if(!e){
		unlock(eg);
		error(Eio);
	}
	lock(e);
	ev = e->val;
	vn = ev? e->val->len : 0;
	if(offset+n > vn)
		n = vn - offset;
	if(n <= 0)
		n = 0;
	else
		memmove(a, ev->dat+offset, n);
	unlock(e);
	unlock(eg);
	return n;
}

long
envwrite(Chan *c, void *va, long n, ulong offset)
{
	Env *e;
	char *p;
	Envval *ev;
	long vn;
	Egrp *eg;
	char *a = va;

	if(n <= 0)
		return 0;
	eg = u->p->egrp;
	lock(eg);
	e = eg->etab[c->qid.path-1].env; /* caller checks for CHDIR */
	if(!e){
		unlock(eg);
		error(Eio);
	}
	lock(&envalloc);
	lock(e);
	if(waserror()){
		unlock(e);
		unlock(&envalloc);
		unlock(eg);
		nexterror();
	}
	if(e->pgref>1)
		panic("envwrite to non-duped env");
	growenval(e, offset+n);
	ev = e->val;
	vn = ev? ev->len : 0;
	if(offset > vn)
		error(Egreg); /* perhaps should zero fill */
	memmove(ev->dat+offset, a, n);
	e->val->len = offset+n;
	poperror();
	unlock(e);
	unlock(&envalloc);
	unlock(eg);
	return n;
}

void
dumpenv(void)
{
	Env *e;
	Envp *ep;
	Envval *ev;
	Egrp *eg;
	int i;
	char hold;

	eg = u->p->egrp;
	for(ep=eg->etab, i=0; i<eg->nenv; i++, ep++)
		print("P%d(%lux %d)",i, ep->env, ep->chref);
	for(e=envalloc.earena; e<&envalloc.earena[conf.nenv]; e++)
		if(e->pgref){
			print("E{%lux %d '%s'}[", e, e->pgref, e->name);
			if(e->val){
				hold = e->val->dat[e->val->len];
				e->val->dat[e->val->len] = 0;
				print("%s", e->val->dat);
				e->val->dat[e->val->len] = hold;
			}
			print("]");
		}else if(e->val)
			print("whoops, free env %lux has val=%lux\n",e,e->val);
	for(i=0, e=envalloc.efree; e; e=e->next)
		i++;
	print("\n%d free envs", i);
	for(i=0, ev=envalloc.arena; ev<envalloc.vfree; ev+=ev->n)
		if(!ev->e)
			i += ev->n*sizeof(Envval);
	print(" %d free enval chars\n", i+((char *)envalloc.end-(char*)envalloc.vfree));
}
