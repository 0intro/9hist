#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

typedef struct	Srv Srv;
struct Srv{
	Lock;
	char	*name;
	Chan	**chan;
}srv;

int
srvgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	if(s >= conf.nsrv)
		return -1;
	if(srv.chan[s] == 0)
		return 0;
	devdir(c, (Qid){s, 0}, &srv.name[s*NAMELEN], 0, eve, 0666, dp);
	return 1;
}

void
srvinit(void)
{
}

void
srvreset(void)
{
	srv.chan = ialloc(conf.nsrv*sizeof(Chan*), 0);
	srv.name = ialloc(conf.nsrv*NAMELEN, 0);
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
	lock(&srv);
	if(waserror()){
		unlock(&srv);
		nexterror();
	}
	f = srv.chan[c->qid.path];
	if(f == 0)
		error(Eshutdown);
	if(omode&OTRUNC)
		error(Eperm);
	if(omode!=f->mode && f->mode!=ORDWR)
		error(Eperm);
	close(c);
	incref(f);
	unlock(&srv);
	poperror();
	return f;
}

void
srvcreate(Chan *c, char *name, int omode, ulong perm)
{
	int j, i;

	if(omode != OWRITE)
		error(Eperm);
	lock(&srv);
	if(waserror()){
		unlock(&srv);
		nexterror();
	}
	j = -1;
	for(i=0; i<conf.nsrv; i++){
		if(srv.chan[i] == 0){
			if(j == -1)
				j = i;
		}else if(strcmp(name, &srv.name[i*NAMELEN]) == 0)
			error(Einuse);
	}
	if(j == -1)
		error(Enosrv);
	srv.chan[j] = c;
	unlock(&srv);
	poperror();
	strcpy(&srv.name[j*NAMELEN], name);
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
	lock(&srv);
	if(waserror()){
		unlock(&srv);
		nexterror();
	}
	f = srv.chan[c->qid.path];
	if(f == 0)
		error(Eshutdown);
	if(strcmp(&srv.name[c->qid.path*NAMELEN], "boot") == 0)
		error(Eperm);
	srv.chan[c->qid.path] = 0;
	unlock(&srv);
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
	int i, fd;
	char buf[32];

	i = c->qid.path;
	if(srv.chan[i] != c)	/* already been written to */
		error(Egreg);
	if(n >= sizeof buf)
		error(Egreg);
	memmove(buf, va, n);	/* so we can NUL-terminate */
	buf[n] = 0;
	fd = strtoul(buf, 0, 0);
	fdtochan(fd, -1, 0);	/* error check only */
	srv.chan[i] = u->p->fgrp->fd[fd];
	incref(srv.chan[i]);
	return n;
}
