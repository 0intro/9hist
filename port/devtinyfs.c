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
	Nfile=		32,
	Qmedium,

	Magic=		0xfeedbeef,
	Superlen=	64,
};

typedef struct FS FS;

struct FS {
	QLock;
	Ref	r;
	int	dev;
	FS	*next;
	Chan	*c;
	uchar	*fat;
	ulong	nclust;
	ulong	clustsize;
};

struct {
	QLock;
	FS	*l;
	int	hidev;
} tinyfs;

void
tinyfsreset(void)
{
}

void
tinyfsinit(void)
{
}

#define GETS(x) ((x)[0]|((x)[1]<<8))
#define PUTS(x, v) {(x)[0] = (v);(x)[1] = ((v)>>8);}

#define GETL(x) (GETS(x)|(GETS(x+2)<<16))
#define PUTL(x, v) {PUTS(x, v);PUTS(x+2, (v)>>16)}; 

/*
 *  see if we have a reasonable fat/root directory
 */
static int
fsinit(FS *fs)
{
	uchar buf[DIRLEN];
	Dir d;
	ulong x;

	n = devtab[fs->c->type].read(fs->c, buf, Superlen, 0);
	if(n != Superlen)
		error(Eio);
	x = GETL(buf);
	if(x != Magic)
		return -1;
	fs->clustsize = GETL(buf+4);
	fs->nclust = GETL(buf+8);
	x = fs->clustsize*fs->nclust;

	devtab[fs->c->type].stat(fs->c, buf);
	convM2D(buf, &d);
	if(d.length < 128)
		error("tinyfs medium too small");
	if(d.length < x)
		return -1;

	fs->fat = smalloc(2*fs->nclust);
	n = devtab[fs->c->type].read(fs->c, buf, 2*fs->nclust, Superlen);
	fd(n != 2*fs->nclust)
		error(Eio);

	x = GETS(fs->fat);
	if(x == 0)
		return -1;

	return 0;
}

/*
 *  set up the fat and then a root directory (starting at first cluster (1))
 */
static void
fssetup(FS *fs)
{
	uchar buf[DIRLEN];
	Dir d;

	devtab[fs->c->type].stat(fs->c, buf);
	convM2D(buf, &d);
	fs->clustsize = d.length>>16;
	if(fs->clustsize < 64)
		fs->clustsize = 64;
	fs->nclust = (d.length - 12)/fs->clustsize;
	fs->fat = smalloc(2*fs->nclust);
	n = devtab[fs->c->type].write(fs->c, buf, 2*fs->nclust, Superlen);
	if(n < 2*fs->nclust)
		error(Eio);
	n = devtab[fs->c->type].write(fs->c, buf, Superlen, 0);
	if(n < Superlen)
		error(Eio);
}

Chan *
tinyfsattach(char *spec)
{
	FS *fs, **l;
	Chan *c, *cc;

	cc = namec((char*)arg[0], Aopen, arg[1], 0);
	if(waserror()){
		close(cc);
		unlock(&fs);
		nexterror();
	}
	qlock(&tinyfs);
	l = &tinyfs.l;
	for(fs = tinyfs.l; fs != 0; fs = fs->next){
		if(eqchan(c, fs->c))
			break;
		l = &(fs->next);
	}
	if(fs){
		incref(&fs->r);
		qunlock(&tinyfs);
		close(cc);
	} else {
		fs = smalloc(sizeof(*fs));
		fs->c = cc;
		incref(&fs->r);
		if(waserror()){
			free(fs);
			nexterror();
		}
		if(fsinit(fs) < 0)
			fssetup(fs);
		poperror();
		*l = fs;
		qunlock(&tinyfs);
	}
	poperror();

	c = devattach('E', spec);
	c->aux = fs;
	c->dev = fs->dev;

	return c;
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
