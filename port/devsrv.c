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
	devdir(c, s, &srv.name[s*NAMELEN], 0, 0666, dp);
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
	return devwalk(c, name, (Dirtab *)0, 0, srvgen);
}

void
srvstat(Chan *c, char *db)
{
	devstat(c, db, (Dirtab *)0, 0L, srvgen);
}

Chan *
srvopen(Chan *c, int omode)
{
	Chan *f;

	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eisdir);
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
	f = srv.chan[c->qid];
	if(f == 0)
		error(0, Eshutdown);
	if(omode & OTRUNC)
		error(0, Eperm);
	if(omode!=f->mode && f->mode!=ORDWR)
		error(0, Eperm);
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
		error(0, Eperm);
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
		}else if(strcmp(name, &srv.name[i*NAMELEN]) == 0){
			print("reuse of srv name\n");
			error(0, Einuse);
		}
	}
	if(j == -1)
		error(0, Enosrv);
	srv.chan[j] = c;
	unlock(&srv);
	strcpy(&srv.name[j*NAMELEN], name);
	c->qid = j;
	c->flag |= COPEN;
	c->mode = OWRITE;
}

void
srvremove(Chan *c)
{
	Chan *f;

	if(c->qid == CHDIR)
		error(0, Eperm);
	lock(&srv);
	if(waserror()){
		unlock(&srv);
		nexterror();
	}
	f = srv.chan[c->qid];
	if(f == 0)
		error(0, Eshutdown);
	if(strcmp(&srv.name[c->qid*NAMELEN], "boot") == 0)
		error(0, Eperm);
	srv.chan[c->qid] = 0;
	unlock(&srv);
	poperror();
	close(f);
}

void
srvwstat(Chan *c, char *dp)
{
	error(0, Egreg);
}

void
srvclose(Chan *c)
{
}

long
srvread(Chan *c, void *va, long n)
{
	char *a = va;

	if(c->qid != CHDIR)
		panic("srvread");
	return devdirread(c, a, n, (Dirtab *)0, 0L, srvgen);
}

long
srvwrite(Chan *c, void *va, long n)
{
	int i, fd;
	char buf[32];

	i = c->qid;
	if(srv.chan[i] != c)	/* already been written to */
		error(0, Egreg);
	if(n >= sizeof buf)
		error(0, Egreg);
	memcpy(buf, va, n);	/* so we can NUL-terminate */
	buf[n] = 0;
	fd = strtoul(buf, 0, 0);
	fdtochan(fd, -1);	/* error check only */
	srv.chan[i] = u->fd[fd];
	incref(u->fd[fd]);
	return n;
}

void
srverrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void
srvuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}
