/*
 *  template for making a new device
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"devtab.h"

enum{
	Qdir,
	Qdata,
};
Dirtab XXXtab[]={
	"data",		{Qdata, 0},	0,	0600,
};
#define NXXXtab (sizeof(XXXtab)/sizeof(Dirtab))

void
XXXreset(void)
{
}

void
XXXinit(void)
{
}

Chan *
XXXattach(char *spec)
{
	return devattach('X', spec);
}

Chan *
XXXclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
XXXwalk(Chan *c, char *name)
{
	return devwalk(c, name, XXXtab, NXXXtab, devgen);
}

void
XXXstat(Chan *c, char *db)
{
	devstat(c, db, XXXtab, NXXXtab, devgen);
}

Chan *
XXXopen(Chan *c, int omode)
{
	return devopen(c, omode, XXXtab, NXXXtab, devgen);
}

void
XXXcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
XXXremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
XXXwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
XXXclose(Chan *c)
{
}

long
XXXread(Chan *c, void *a, long n, ulong offset)
{
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, XXXtab, NXXXtab, devgen);
	case Qdata:
		break;
	default:
		n=0;
		break;
	}
	return n;
}

Block*
XXXbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
XXXwrite(Chan *c, char *a, long n, ulong offset)
{
	switch(c->qid.path & ~CHDIR){
	case Qdata:
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

long
XXXbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}
