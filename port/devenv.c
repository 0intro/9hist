#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

struct Envval
{
	Envval	*next;		/* for hashing & easy deletion from hash list */
	Envval	*prev;
	ulong	len;		/* length of val that is valid */
	int	ref;
	char	*val;
};

enum{
	MAXENV	= (BY2PG - sizeof(Envval)),
	EVHASH	= 64,
	EVFREE	= 16,
	ALIGN	= 16,
};

struct
{
	Envval	*free[EVFREE+1];
	char	*block;			/* the free page we are allocating from */
	char	*lim;			/* end of block */
	int	npage;			/* total pages gotten from newpage() */
}envalloc;

QLock	evlock;
Envval	evhash[EVHASH];
char	*evscratch;		/* for constructing the contents of a file */

Envval	*newev(char*, ulong);
Envval	*evalloc(ulong);
void	evfree(Envval*);

void
envreset(void)
{
	evscratch = ialloc(BY2PG, 0);
}

void
envinit(void)
{
}

int
envgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Egrp *eg;
	Env *e;
	int ans;

	eg = u->p->egrp;
	qlock(&eg->ev);
	if(s >= eg->nenv)
		ans = -1;
	else{
		e = &eg->etab[s];
		if(!e->name)
			ans = 0;
		else{
			devdir(c, (Qid){s+1, (ulong)e->val}, e->name->val, e->val? e->val->len : 0, eve, 0666, dp);
			ans = 1;
		}
	}
	qunlock(&eg->ev);
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
	return devclone(c, nc);
}

int
envwalk(Chan *c, char *name)
{

	return devwalk(c, name, 0, 0, envgen);
}

void
envstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, envgen);
}

Chan *
envopen(Chan *c, int omode)
{
	Egrp *eg;
	Env *e;
	int mode;

	mode = openmode(omode);
	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}else{
		eg = u->p->egrp;
		qlock(&eg->ev);
		e = &eg->etab[c->qid.path-1];
		if(!e->name){
			qunlock(&eg->ev);
			error(Enonexist);
		}
		if(omode == (OWRITE|OTRUNC) && e->val){
			qlock(&evlock);
			evfree(e->val);
			qunlock(&evlock);
			e->val = 0;
		}
		qunlock(&eg->ev);
	}
	c->mode = mode;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
envcreate(Chan *c, char *name, int omode, ulong perm)
{
	Egrp *eg;
	Env *e, *ne;
	int i;

	if(c->qid.path != CHDIR)
		error(Eperm);
	omode = openmode(omode);
	eg = u->p->egrp;
	qlock(&eg->ev);
	e = eg->etab;
	ne = 0;
	for(i = 0; i < eg->nenv; i++, e++)
		if(e->name == 0)
			ne = e;
		else if(strcmp(e->name->val, name) == 0){
			qunlock(&eg->ev);
			error(Einuse);
		}
	if(ne)
		e = ne;
	else if(eg->nenv == conf.npgenv){
		qunlock(&eg->ev);
		print("out of egroup envs\n");
		error(Enoenv);
	}
	i = e - eg->etab + 1;
	e->val = 0;
	qlock(&evlock);
	e->name = newev(name, strlen(name)+1);
	qunlock(&evlock);
	if(i > eg->nenv)
		eg->nenv = i;
	qunlock(&eg->ev);
	c->qid = (Qid){i, 0};
	c->offset = 0;
	c->mode = omode;
	c->flag |= COPEN;
}

void
envremove(Chan *c)
{
	Egrp *eg;
	Env *e;

	if(c->qid.path & CHDIR)
		error(Eperm);
	eg = u->p->egrp;
	qlock(&eg->ev);
	e = &eg->etab[c->qid.path-1];
	if(!e->name){
		qunlock(&eg->ev);
		error(Enonexist);
	}
	envpgclose(e);
	qunlock(&eg->ev);
}

void
envwstat(Chan *c, char *db)
{
	USED(c, db);
	error(Eperm);
}

void
envclose(Chan * c)
{
	USED(c);
}

void
envpgcopy(Env *t, Env *f)
{
	qlock(&evlock);
	if(t->name = f->name)
		t->name->ref++;
	if(t->val = f->val)
		t->val->ref++;
	qunlock(&evlock);
}

void
envpgclose(Env *e)
{
	qlock(&evlock);
	if(e->name)
		evfree(e->name);
	if(e->val)
		evfree(e->val);
	e->name = e->val = 0;
	qunlock(&evlock);
}

long
envread(Chan *c, void *a, long n, ulong offset)
{
	Egrp *eg;
	Env *e;
	Envval *ev;
	long vn;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, envgen);
	eg = u->p->egrp;
	qlock(&eg->ev);
	e = &eg->etab[c->qid.path-1];
	if(!e->name){
		qunlock(&eg->ev);
		error(Enonexist);
	}
	ev = e->val;
	vn = ev ? ev->len : 0;
	if(offset + n > vn)
		n = vn - offset;
	if(n <= 0)
		n = 0;
	else
		memmove(a, ev->val + offset, n);
	qunlock(&eg->ev);
	return n;
}

long
envwrite(Chan *c, void *a, long n, ulong offset)
{
	Egrp *eg;
	Env *e;
	Envval *ev;
	ulong olen;

	if(n <= 0)
		return 0;
	olen = (offset + n + ALIGN - 1) & ~(ALIGN - 1);
	if(olen > MAXENV)
		error(Etoobig);
	eg = u->p->egrp;
	qlock(&eg->ev);
	e = &eg->etab[c->qid.path-1];
	if(!e->name){
		qunlock(&eg->ev);
		error(Enonexist);
	}
	ev = e->val;
	olen = ev ? ev->len : 0;
	qlock(&evlock);
	if(offset == 0 && n >= olen)
		e->val = newev(a, n);
	else{
		if(olen > offset)
			olen = offset;
		if(ev)
			memmove(evscratch, ev->val, olen);
		if(olen < offset)
			memset(evscratch + olen, '\0', offset - olen);
		memmove(evscratch + offset, a, n);
		e->val = newev(evscratch, offset + n);
	}
	if(ev)
		evfree(ev);
	qunlock(&evlock);
	qunlock(&eg->ev);
	return n;
}

/*
 * called with evlock qlocked
 */
Envval *
newev(char *s, ulong n)
{
	Envval *ev;
	uchar *t;
	int h;

	h = 0;
	for(t = (uchar*)s; t - (uchar*)s < n; t++)
		h = (h << 1) ^ *t;
	h &= EVHASH - 1;
	for(ev = evhash[h].next; ev; ev = ev->next)
		if(ev->len == n && memcmp(ev->val, s, n) == 0){
			ev->ref++;
			return ev;
		}
	ev = evalloc(n);
	ev->len = n;
	memmove(ev->val, s, n);
	if(ev->next = evhash[h].next)
		ev->next->prev = ev;
	evhash[h].next = ev;
	ev->prev = &evhash[h];
	return ev;
}

/*
 * called only from newev
 */
Envval *
evalloc(ulong n)
{
	Envval *ev, **p;
	char *b, *lim;
	ulong size;

	size = (n + ALIGN - 1) & ~(ALIGN - 1);
	n = (size - 1) / ALIGN;
	p = &envalloc.free[n < EVFREE ? n : EVFREE];
	for(ev = *p; ev; ev = *p){
		if(ev->len == size){
			*p = ev->next;
			ev->ref = 1;
			return ev;
		}
		p = &ev->next;
	}

	/*
	 * make sure we have enough space to allocate the buffer.
	 * if not, use the remaining space for the smallest buffers
	 */
	if(size > MAXENV)
		panic("evalloc");
	b = envalloc.block;
	lim = envalloc.lim;
	if(!b || lim < b + size + sizeof *ev){
		p = &envalloc.free[0];
		while(lim >= b + ALIGN + sizeof *ev){
			ev = (Envval*)b;
			ev->len = ALIGN;
			ev->val = b + sizeof *ev;
			ev->next = *p;
			*p = ev;
			b += ALIGN + sizeof *ev;
		}
		b = (char*)VA(kmap(newpage(0, 0, 0)));
		envalloc.npage++;
		envalloc.lim = b + BY2PG;
	}
	
	ev = (Envval*)b;
	ev->val = b + sizeof *ev;
	ev->ref = 1;
	envalloc.block = b + size + sizeof *ev;
	return ev;
}

/*
 * called with evlock qlocked
 */
void
evfree(Envval *ev)
{
	int n;

	if(--ev->ref > 0)
		return;

	if(ev->prev)
		ev->prev->next = ev->next;
	else
		panic("evfree");
	if(ev->next)
		ev->next->prev = ev->prev;
	n = (ev->len + ALIGN - 1) & ~(ALIGN - 1);
	ev->len = n;
	n = (n - 1) / ALIGN;
	if(n > EVFREE)
		n = EVFREE;
	ev->next = envalloc.free[n];
	ev->prev = 0;
	envalloc.free[n] = ev;
}

/*
 *  to let the kernel set environment variables
 */
void
ksetenv(char *ename, char *eval)
{
	Chan *c;
	char buf[2*NAMELEN];

	sprint(buf, "#e/%s", ename);
	c = namec(buf, Acreate, OWRITE, 0600);
	(*devtab[c->type].write)(c, eval, strlen(eval), 0);
	close(c);
}

void
ksetterm(char *f)
{
	char buf[2*NAMELEN];

	sprint(buf, f, conffile);
	ksetenv("terminal", buf);
}
