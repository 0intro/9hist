#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

struct
{
	Lock;
	int	fid;
	Chan	*free;
	Chan	*list;
}chanalloc;

int
incref(Ref *r)
{
	int x;

	lock(r);
	x = ++r->ref;
	unlock(r);
	return x;
}

int
decref(Ref *r)
{
	int x;

	lock(r);
	x = --r->ref;
	unlock(r);
	if(x < 0)
		panic("decref");

	return x;
}

void
chanrec(Mnt *m)
{
	Chan *c;

	lock(&chanalloc);
	for(c = chanalloc.list; c; c = c->link)
		if(c->mntptr == m)
			c->flag |= CRECOV;
	unlock(&chanalloc);
}

void
chandevreset(void)
{
	int i;

	for(i=0; devtab[i] != nil; i++)
		devtab[i]->reset();
}

void
chandevinit(void)
{
	int i;

	for(i=0; devtab[i] != nil; i++)
		devtab[i]->init();
}

Chan*
newchan(void)
{
	Chan *c;

	lock(&chanalloc);
	c = chanalloc.free;
	if(c != 0)
		chanalloc.free = c->next;
	unlock(&chanalloc);

	if(c == 0) {
		c = smalloc(sizeof(Chan));
		lock(&chanalloc);
		c->fid = ++chanalloc.fid;
		c->link = chanalloc.list;
		chanalloc.list = c;
		unlock(&chanalloc);
	}

	/* if you get an error before associating with a dev,
	   close calls rootclose, a nop */
	c->type = 0;
	c->flag = 0;
	c->ref = 1;
	c->dev = 0;
	c->offset = 0;
	c->mh = 0;
	c->xmh = 0;
	c->uri = 0;
	c->aux = 0;
	c->mchan = 0;
	c->path = 0;
	c->mcp = 0;
	c->mqid = (Qid){0, 0};
	return c;
}

void
chanfree(Chan *c)
{
	c->flag = CFREE;

	if(c->session){
		freesession(c->session);
		c->session = 0;
	}

	if(c->mh != nil){
		putmhead(c->mh);
		c->mh = nil;
	}

	/*
	 * Channel can be closed before a path is created or the last
	 * channel in a mount which has already cleared its pt names
	 */
	if(c->path) {
		if (c->path->ref <= 0) {
			char buf[100];
			ptpath(c->path, buf, sizeof(buf));
			print("decref %s\n", buf);
		}
		decref(c->path);
	}

	lock(&chanalloc);
	c->next = chanalloc.free;
	chanalloc.free = c;
	unlock(&chanalloc);
}

void
cclose(Chan *c)
{
	if(c->flag&CFREE)
		panic("cclose %lux", getcallerpc(c));
	if(decref(c))
		return;

	if(!waserror()) {
		devtab[c->type]->close(c);
		poperror();
	}

	chanfree(c);
}

int
eqqid(Qid a, Qid b)
{
	return a.path==b.path && a.vers==b.vers;
}

int
eqchan(Chan *a, Chan *b, int pathonly)
{
	if(a->qid.path != b->qid.path)
		return 0;
	if(!pathonly && a->qid.vers!=b->qid.vers)
		return 0;
	if(a->type != b->type)
		return 0;
	if(a->dev != b->dev)
		return 0;
	return 1;
}

int
cmount(Chan *new, Chan *old, int flag, char *spec)
{
	Pgrp *pg;
	int order, flg;
	Mhead *m, **l;
	Mount *nm, *f, *um, **h;

	if(CHDIR & (old->qid.path^new->qid.path))
		error(Emount);

	order = flag&MORDER;

	if((old->qid.path&CHDIR)==0 && order != MREPL)
		error(Emount);

	pg = up->pgrp;
	wlock(&pg->ns);

	l = &MOUNTH(pg, old);
	for(m = *l; m; m = m->hash) {
		if(eqchan(m->from, old, 1))
			break;
		l = &m->hash;
	}

	if(m == 0) {
		/*
		 *  nothing mounted here yet.  create a mount
		 *  head and add to the hash table.
		 */
		m = smalloc(sizeof(Mhead));
		m->ref = 1;
		m->from = old;
		incref(old);
		m->hash = *l;
		*l = m;

		/*
		 *  if this is a union mount, add the old
		 *  node to the mount chain.
		 */
		if(order != MREPL)
			m->mount = newmount(m, old, 0, 0);
	}
	wlock(&m->lock);
	if(waserror()){
		wunlock(&m->lock);
		nexterror();
	}
	wunlock(&pg->ns);

	nm = newmount(m, new, flag, spec);
	if(new->mh != nil && new->mh->mount != nil) {
		/*
		 *  copy a union when binding it onto a directory
		 */
		flg = order;
		if(order == MREPL)
			flg = MAFTER;
		h = &nm->next;
		um = new->mh->mount;
		for(um = um->next; um; um = um->next) {
			f = newmount(m, um->to, flg, um->spec);
			*h = f;
			h = &f->next;
		}
	}

	if(m->mount && order == MREPL) {
		mountfree(m->mount);
		m->mount = 0;
	}

	if(flag & MCREATE)
		new->flag |= CCREATE;

	if(m->mount && order == MAFTER) {
		for(f = m->mount; f->next; f = f->next)
			;
		f->next = nm;
	}
	else {
		for(f = nm; f->next; f = f->next)
			;
		f->next = m->mount;
		m->mount = nm;
	}

	wunlock(&m->lock);
	poperror();
	return nm->mountid;
}

void
cunmount(Chan *mnt, Chan *mounted)
{
	Pgrp *pg;
	Mhead *m, **l;
	Mount *f, **p;

	pg = up->pgrp;
	wlock(&pg->ns);

	l = &MOUNTH(pg, mnt);
	for(m = *l; m; m = m->hash) {
		if(eqchan(m->from, mnt, 1))
			break;
		l = &m->hash;
	}

	if(m == 0) {
		wunlock(&pg->ns);
		error(Eunmount);
	}

	wlock(&m->lock);
	if(mounted == 0) {
		*l = m->hash;
		wunlock(&pg->ns);
		mountfree(m->mount);
		m->mount = nil;
		cclose(m->from);
		wunlock(&m->lock);
		putmhead(m);
		return;
	}
	wunlock(&pg->ns);

	p = &m->mount;
	for(f = *p; f; f = f->next) {
		/* BUG: Needs to be 2 pass */
		if(eqchan(f->to, mounted, 1) ||
		  (f->to->mchan && eqchan(f->to->mchan, mounted, 1))) {
			*p = f->next;
			f->next = 0;
			mountfree(f);
			if(m->mount == nil) {
				*l = m->hash;
				wunlock(&pg->ns);
				cclose(m->from);
				wunlock(&m->lock);
				putmhead(m);
				return;
			}
			wunlock(&m->lock);
			return;
		}
		p = &f->next;
	}
	wunlock(&m->lock);
	error(Eunion);
}

Chan*
cclone(Chan *c, Chan *nc)
{
	return devtab[c->type]->clone(c, nc);
}

Chan*
domount(Chan *c)
{
	Pgrp *pg;
	Chan *nc;
	Mhead *m;

	pg = up->pgrp;
	rlock(&pg->ns);
	if(c->mh){
		putmhead(c->mh);
		c->mh = 0;
	}

	for(m = MOUNTH(pg, c); m; m = m->hash){
		rlock(&m->lock);
		if(eqchan(m->from, c, 1)) {
			if(waserror()) {
				runlock(&m->lock);
				nexterror();
			}
			runlock(&pg->ns);
			nc = cclone(m->mount->to, 0);
			if(nc->mh != nil)
				putmhead(nc->mh);
			nc->mh = m;
			nc->xmh = m;
			incref(m);
			cclose(c);
			c = nc;
			poperror();
			runlock(&m->lock);
			return c;
		}
		runlock(&m->lock);
	}

	runlock(&pg->ns);
	return c;
}

Chan*
undomount(Chan *c)
{
	Chan *nc;
	Pgrp *pg;
	Mount *t;
	Mhead **h, **he, *f;

	pg = up->pgrp;
	rlock(&pg->ns);
	if(waserror()) {
		runlock(&pg->ns);
		nexterror();
	}

	he = &pg->mnthash[MNTHASH];
	for(h = pg->mnthash; h < he; h++) {
		for(f = *h; f; f = f->hash) {
			for(t = f->mount; t; t = t->next) {
				if(eqchan(c, t->to, 1)) {
					nc = cclone(t->head->from, 0);
					cclose(c);
					c = nc;
					break;
				}
			}
		}
	}
	poperror();
	runlock(&pg->ns);
	return c;
}

int
walk(Chan **cp, char *name, int domnt)
{
	Chan *c, *ac;
	Mount *f;
	int dotdot;

	ac = *cp;

	if(name[0] == '\0')
		return 0;

	dotdot = 0;
	if(name[0] == '.' && name[1] == '.' && name[2] == '\0') {
		*cp = ac = undomount(ac);
		dotdot = 1;
	}

	ac->flag &= ~CCREATE;	/* not inherited through a walk */
	if(devtab[ac->type]->walk(ac, name) != 0) {
		if(dotdot)
			*cp = undomount(*cp);
		if(domnt)
			*cp = domount(*cp);
		return 0;
	}

	if(ac->mh == nil)
		return -1;

	c = nil;

	rlock(&ac->mh->lock);
	if(waserror()) {
		runlock(&ac->mh->lock);
		if(c)
			cclose(c);
		nexterror();
	}
	for(f = ac->mh->mount; f; f = f->next) {
		c = cclone(f->to, 0);
		c->flag &= ~CCREATE;	/* not inherited through a walk */
		if(devtab[c->type]->walk(c, name) != 0)
			break;
		cclose(c);
		c = nil;
	}
	poperror();
	runlock(&ac->mh->lock);

	if(c == nil)
		return -1;

	if(dotdot)
		c = undomount(c);

	if(c->mh){
		putmhead(c->mh);
		c->mh = nil;
	}
	if(domnt) {
		if(waserror()) {
			cclose(c);
			nexterror();
		}
		c = domount(c);
		poperror();
	}
	cclose(ac);
	*cp = c;
	return 0;
}

/*
 * c is a mounted non-creatable directory.  find a creatable one.
 */
Chan*
createdir(Chan *c)
{
	Chan *nc;
	Mount *f;

	rlock(&c->mh->lock);
	if(waserror()) {
		runlock(&c->mh->lock);
		nexterror();
	}
	for(f = c->mh->mount; f; f = f->next) {
		if(f->to->flag&CCREATE) {
			nc = cclone(f->to, 0);
			if(nc->mh != nil)
				putmhead(nc->mh);
			nc->mh = c->mh;
			incref(c->mh);
			runlock(&c->mh->lock);
			poperror();
			cclose(c);
			return nc;
		}
	}
	error(Enocreate);
	return 0;
}

Chan*
mchan(char *id, int walkname)
{
	Chan *c;
	Pgrp *pg;
	Mount *t;
	int mdev;
	ulong mountid;
	Mhead **h, **he, *f;

	mountid = strtoul(id, 0, 0);
	mdev = devno('M', 0);

	pg = up->pgrp;
	rlock(&pg->ns);
	if(waserror()) {
		runlock(&pg->ns);
		nexterror();
	}

	he = &pg->mnthash[MNTHASH];
	for(h = pg->mnthash; h < he; h++) {
		for(f = *h; f; f = f->hash) {
			for(t = f->mount; t; t = t->next) {
				c = t->to;
				if(c->type == mdev && c->mntptr->id == mountid) {
					if(walkname == 0) {
						c = c->mntptr->c;
						incref(c);
					}
					else
						c = cclone(c, 0);
					runlock(&pg->ns);
					poperror();
					return c;
				}
			}
		}
	}
	error(Enonexist);
	return 0;
}

void
saveregisters(void)
{
}

/*
 * Turn a name into a channel.
 * &name[0] is known to be a valid address.  It may be a kernel address.
 */
Chan*
namec(char *name, int amode, int omode, ulong perm)
{
	Rune r;
	char *p;
	char *elem;
	int t, n;
	int mntok, isdot;
	Chan *c, *cc;
	char createerr[ERRLEN];

	if(name[0] == 0)
		error(Enonexist);

	if(!((ulong)name & KZERO)) {
		p = name;
		t = BY2PG-((ulong)p&(BY2PG-1));
		while(vmemchr(p, 0, t) == 0) {
			p += t;
			t = BY2PG;
		}
	}

	elem = up->elem;
	mntok = 1;
	isdot = 0;
	switch(name[0]) {
	case '/':
		c = cclone(up->slash, 0);
		name = skipslash(name);
		break;
	case '#':
		mntok = 0;
		elem[0] = 0;
		n = 0;
		while(*name && (*name != '/' || n < 2))
			elem[n++] = *name++;
		elem[n] = '\0';
		n = chartorune(&r, elem+1)+1;
		if(r == 'M') {
			if(elem[n] == 'c') {
				c = mchan(elem+n+1, 0);
				name = skipslash(name);
				if(*name)
					error(Efilename);
				return c;
			}
			else {
				c = mchan(elem+n, 1);
				name = skipslash(name);
			}
			break;
		}
		t = devno(r, 1);
		if(t == -1)
			error(Ebadsharp);

		c = devtab[t]->attach(elem+n);
		name = skipslash(name);
		break;
	default:
		c = cclone(up->dot, 0);
		name = skipslash(name);
		if(*name == 0)
			isdot = 1;
	}

	if(waserror()){
		cclose(c);
		nexterror();
	}

	name = nextelem(name, elem);

	/*
	 *  If mounting, don't follow the mount entry for root or the
	 *  current directory.
	 */
	if(mntok && !isdot && !(amode==Amount && elem[0]==0))
		c = domount(c);			/* see case Atodir below */

	while(*name) {
		if(walk(&c, elem, mntok) < 0)
			error(Enonexist);
		name = nextelem(name, elem);
	}

	switch(amode) {
	case Aaccess:
		if(isdot) {
			c = domount(c);
			break;
		}
		if(walk(&c, elem, mntok) < 0)
			error(Enonexist);
		break;

	case Atodir:
		/*
		 * Directories (e.g. for cd) are left before the mount point,
		 * so one may mount on / or . and see the effect.
		 */
		if(walk(&c, elem, 0) < 0)
			error(Enonexist);
		if(!(c->qid.path & CHDIR))
			error(Enotdir);
		break;

	case Aopen:
		if(isdot)
			c = domount(c);
		else {
			if(walk(&c, elem, mntok) < 0)
				error(Enonexist);
		}
	Open:
		/* else error() in open has wrong value of c saved */
		saveregisters();

		if(omode == OEXEC)
			c->flag &= ~CCACHE;

		c = devtab[c->type]->open(c, omode&~OCEXEC);

		if(omode & OCEXEC)
			c->flag |= CCEXEC;
		if(omode & ORCLOSE)
			c->flag |= CRCLOSE;
		break;

	case Amount:
		/*
		 * When mounting on an already mounted upon directory,
		 * one wants subsequent mounts to be attached to the
		 * original directory, not the replacement.
		 */
		if(walk(&c, elem, 0) < 0)
			error(Enonexist);
		break;

	case Acreate:
		if(isdot)
			error(Eisdir);

		/*
		 *  Walk the element before trying to create it
		 *  to see if it exists.  We clone the channel
		 *  first, just in case someone is trying to
		 *  use clwalk outside the kernel.
		 */
		cc = cclone(c, 0);
		if(waserror()){
			cclose(cc);
			nexterror();
		}
		nameok(elem);
		if(walk(&cc, elem, 1) == 0){
			poperror();
			cclose(c);
			c = cc;
			omode |= OTRUNC;
			goto Open;
		}
		cclose(cc);
		poperror();

		/*
		 *  the file didn't exist, try the create
		 */
		if(c->mh != nil && !(c->flag&CCREATE))
			c = createdir(c);

		/*
		 * protect against the open/create race.
		 * This is not a complete fix. It just reduces the window.
		 */
		if(waserror()) {
			strcpy(createerr, up->error);
			if(walk(&c, elem, 1) < 0)
				error(createerr);
			omode |= OTRUNC;
			goto Open;
		}
		devtab[c->type]->create(c, elem, omode&~OCEXEC, perm);
		if(omode & OCEXEC)
			c->flag |= CCEXEC;
		if(omode & ORCLOSE)
			c->flag |= CRCLOSE;
		poperror();
		break;

	default:
		panic("unknown namec access %d\n", amode);
	}
	poperror();
	return c;
}

/*
 * name[0] is addressable.
 */
char*
skipslash(char *name)
{
    Again:
	while(*name == '/')
		name++;
	if(*name=='.' && (name[1]==0 || name[1]=='/')){
		name++;
		goto Again;
	}
	return name;
}

char isfrog[256]={
	/*NUL*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*BKS*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*DLE*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*CAN*/	1, 1, 1, 1, 1, 1, 1, 1,
/*	[' ']	1,	rob - let's try this out */
	['/']	1,
	[0x7f]	1,
};

void
nameok(char *elem)
{
	char *eelem;

	eelem = elem+NAMELEN;
	while(*elem) {
		if(isfrog[*(uchar*)elem])
			error(Ebadchar);
		elem++;
		if(elem >= eelem)
			error(Efilename);
	}
}

/*
 * name[0] should not be a slash.
 */
char*
nextelem(char *name, char *elem)
{
	int w;
	char *end;
	Rune r;

	if(*name == '/')
		error(Efilename);
	end = utfrune(name, '/');
	if(end == 0)
		end = strchr(name, 0);
	w = end-name;
	if(w >= NAMELEN)
		error(Efilename);
	memmove(elem, name, w);
	elem[w] = 0;
	while(name < end){
		name += chartorune(&r, name);
		if(r<sizeof(isfrog) && isfrog[r])
			error(Ebadchar);
	}
	return skipslash(name);
}

void
isdir(Chan *c)
{
	if(c->qid.path & CHDIR)
		return;
	error(Enotdir);
}

void
putmhead(Mhead *m)
{
	if(decref(m) == 0)
		free(m);
}
