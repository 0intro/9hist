#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"devtab.h"

typedef struct	Srv Srv;
struct Srv{
	char	name[NAMELEN];
	char	owner[NAMELEN];
	ulong	perm;
	Chan	*chan;
};

Lock	srvlk;
Srv	*srv;

int
srvgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Srv *sp;

	if(s >= conf.nsrv)
		return -1;

	sp = &srv[s];
	if(sp->chan == 0)
		return 0;
	devdir(c, (Qid){s, 0}, sp->name, 0, sp->owner, sp->perm, dp);
	return 1;
}

void
srvinit(void)
{
}

void
srvreset(void)
{
	srv = ialloc(conf.nsrv*sizeof(Srv), 0);
}

Chan *
srvattach(char *spec)
{
	return devattach('s', spec);
}

Chan *
srvclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
srvwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, srvgen);
}

void
srvstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, srvgen);
}

Chan *
srvopen(Chan *c, int omode)
{
	Chan *f;

	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eisdir);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}
	lock(&srvlk);
	if(waserror()){
		unlock(&srvlk);
		nexterror();
	}
	f = srv[c->qid.path].chan;
	if(f == 0)
		error(Eshutdown);
	if(omode&OTRUNC)
		error(Eperm);
	if(omode!=f->mode && f->mode!=ORDWR)
		error(Eperm);
	close(c);
	incref(f);
	unlock(&srvlk);
	poperror();
	return f;
}

void
srvcreate(Chan *c, char *name, int omode, ulong perm)
{
	int j, i;
	Srv *sp;

	if(omode != OWRITE)
		error(Eperm);

	lock(&srvlk);
	if(waserror()){
		unlock(&srvlk);
		nexterror();
	}
	j = -1;
	for(i=0; i<conf.nsrv; i++){
		if(srv[i].chan == 0){
			if(j == -1)
				j = i;
		}
		else if(strcmp(name, srv[i].name) == 0)
			error(Einuse);
	}
	if(j == -1)
		exhausted("server slots");
	sp = &srv[j];
	sp->chan = c;
	unlock(&srvlk);
	poperror();
	strncpy(sp->name, name, NAMELEN);
	strncpy(sp->owner, u->p->user, NAMELEN);
	sp->perm = perm&0777;

	c->qid.path = j;
	c->flag |= COPEN;
	c->mode = OWRITE;
}

void
srvremove(Chan *c)
{
	Chan *f;

	if(c->qid.path == CHDIR)
		error(Eperm);

	lock(&srvlk);
	if(waserror()){
		unlock(&srvlk);
		nexterror();
	}
	f = srv[c->qid.path].chan;
	if(f == 0)
		error(Eshutdown);
	if(strcmp(srv[c->qid.path].name, "boot") == 0)
		error(Eperm);
	srv[c->qid.path].chan = 0;
	unlock(&srvlk);
	poperror();
	close(f);
}

void
srvwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Egreg);
}

void
srvclose(Chan *c)
{
	USED(c);
}

long
srvread(Chan *c, void *va, long n, ulong offset)
{
	isdir(c);
	return devdirread(c, va, n, 0, 0, srvgen);
}

long
srvwrite(Chan *c, void *va, long n, ulong offset)
{
	Fgrp *f;
	int i, fd;
	char buf[32];

	i = c->qid.path;
	if(srv[i].chan != c)	/* already been written to */
		error(Egreg);
	if(n >= sizeof buf)
		error(Egreg);
	memmove(buf, va, n);	/* so we can NUL-terminate */
	buf[n] = 0;
	fd = strtoul(buf, 0, 0);
	f = u->p->fgrp;
	lock(f);
	if(waserror()){
		unlock(f);
		nexterror();
	}
	fdtochan(fd, -1, 0);	/* error check only */
	srv[i].chan = f->fd[fd];
	incref(srv[i].chan);
	unlock(f);
	poperror();
	return n;
}
