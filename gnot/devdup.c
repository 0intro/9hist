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
	devdir(c, (Qid){s, 0}, buf, 0, perm[f->mode&3], dp);
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

	if(c->qid.path == CHDIR){
		if(omode != 0)
			error(Eisdir);
		c->mode = 0;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}
	fdtochan(c->qid.path, openmode(omode));	/* error check only */
	f = u->fd[c->qid.path];
	close(c);
	incref(f);
	return f;
}

void
dupcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
dupremove(Chan *c)
{
	error(Eperm);
}

void
dupwstat(Chan *c, char *dp)
{
	error(Egreg);
}

void
dupclose(Chan *c)
{
}

long
dupread(Chan *c, void *va, long n)
{
	char *a = va;

	if(c->qid.path != CHDIR)
		panic("dupread");
	return devdirread(c, a, n, (Dirtab *)0, 0L, dupgen);
}

long
dupwrite(Chan *c, void *va, long n)
{
	panic("dupwrite");
}
