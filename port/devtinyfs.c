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
	Qmedium,

	Nfile=	32,
};


struct {
	QLock;
	Chan	*c;
	Dirtab	file[Nfile];
	int	nfile;
} tinyfs;

void
tinyfsreset(void)
{
	Dirtab *d;

	d = tinyfs.file;
	memmove(d->name, "medium");
	d->qid.vers = 0;
	d->qid.path = Qdata;
	d->perm = 0666;
}

void
tinyfsinit(void)
{
}

Chan *
tinyfsattach(char *spec)
{
	return devattach('E', spec);
}

Chan *
tinyfsclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
tinyfswalk(Chan *c, char *name)
{
	return devwalk(c, name, tinyfs.file, tinyfs.nfile, devgen);
}

void
tinyfsstat(Chan *c, char *db)
{
	devstat(c, db, tinyfs.file, tinyfs.nfile, devgen);
}

Chan *
tinyfsopen(Chan *c, int omode)
{
	return devopen(c, omode, tinyfs.file, tinyfs.nfile, devgen);
}

void
tinyfscreate(Chan *c, char *name, int omode, ulong perm)
{
	Dirtab	*d;

	if(perm & CHDIR)
		error("directory creation illegal");

	if(waserror()){
		qunlock(&tinyfs);
		nexterror();
	}
	qlock(&tinyfs);

	if(tinyfs.nfile == Nfile)
		error("out of space");
	for(d = tinyfs.file; d < tinyfs.file[tinyfs.nfile]; d++)
		if(strcmp(name, d->name) == 0)
			error("create race");
	strncpy(d->name, name, sizeof(d->name)-1);
	d->perm = perm;
	d->qid.vers = 0;
	d->qid.path = tinyfs.high++;
	tinyfs.nfile++;
			
	qunlock(&tinyfs);

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->qid = d->qid;
}

void
tinyfsremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
tinyfswstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
tinyfsclose(Chan *c)
{
}

long
tinyfsread(Chan *c, void *a, long n, ulong offset)
{
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, tinyfstab, Ntinyfstab, devgen);
	case Qdata:
		break;
	default:
		n=0;
		break;
	}
	return n;
}

Block*
tinyfsbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
tinyfswrite(Chan *c, char *a, long n, ulong offset)
{
	if(waserror()){
		qunlock(&tinyfs);
		nexterror();
	}
	qlock(&tinyfs);
	qunlock(&tinyfs);

	switch(c->qid.path & ~CHDIR){
	case Qdata:
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

long
tinyfsbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}
