#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"


int
dupgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	char buf[8];
	Chan *f;
	static int perm[] = { 0400, 0200, 0600, 0 };

	if(s >= NFD)
		return -1;
	if((f=u->fd[s]) == 0)
		return 0;
	sprint(buf, "%ld", s);
	devdir(c, s, buf, 0, perm[f->mode&3], dp);
	return 1;
}

void
dupinit(void)
{
}

void
dupreset(void)
{
}

Chan *
dupattach(char *spec)
{
	return devattach('d', spec);
}

Chan *
dupclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
dupwalk(Chan *c, char *name)
{
	return devwalk(c, name, (Dirtab *)0, 0, dupgen);
}

void
dupstat(Chan *c, char *db)
{
	devstat(c, db, (Dirtab *)0, 0L, dupgen);
}

Chan *
dupopen(Chan *c, int omode)
{
	Chan *f;

	if(c->qid == CHDIR){
		if(omode != 0)
			error(0, Eisdir);
		c->mode = 0;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}
	fdtochan(c->qid, openmode(omode));	/* error check only */
	f = u->fd[c->qid];
	close(c);
	incref(f);
	return f;
}

void
dupcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void
dupremove(Chan *c)
{
	error(0, Eperm);
}

void
dupwstat(Chan *c, char *dp)
{
	error(0, Egreg);
}

void
dupclose(Chan *c)
{
}

long
dupread(Chan *c, void *va, long n)
{
	char *a = va;

	if(c->qid != CHDIR)
		panic("dupread");
	return devdirread(c, a, n, (Dirtab *)0, 0L, dupgen);
}

long
dupwrite(Chan *c, void *va, long n)
{
	panic("dupwrite");
}

void
duperrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void
dupuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}
