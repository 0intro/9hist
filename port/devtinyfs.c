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

	Blen=	48,

	Tdir=	0,
	Tdata,
	Tend,
};

typedef struct FS FS;
struct FS {
	QLock;
	Ref	r;
	int	dev;
	FS	*next;
	Chan	*c;
	uchar	*map;
	int	nblocks;
};

struct {
	QLock;
	FS	*l;
	int	hidev;
} tinyfs;

#define GETS(x) ((x)[0]|((x)[1]<<8))
#define PUTS(x, v) {(x)[0] = (v);(x)[1] = ((v)>>8);}

#define GETL(x) (GETS(x)|(GETS(x+2)<<16))
#define PUTL(x, v) {PUTS(x, v);PUTS(x+2, (v)>>16)};

void
tinyfsreset(void)
{
}

void
tinyfsinit(void)
{
}

static uchar
checksum(uchar *p)
{
	uchar *e;
	uchar s;

	s = 0;
	for(e = p + Blen; p < e; p++)
		s += *p;
}

static void
mapclr(FS *fs, int bno)
{
	fs->map[bno>>3] &= ~(1<<(bno&7));
}

static void
mapset(FS *fs, int bno)
{
	fs->map[bno>>3] |= 1<<(bno&7);
}

static int
mapalloc(FS *fs)
{
	int i, j, lim;
	uchar x;

	qlock(fs);
	lim = (fs->nblocks + 8 - 1)/8;
	for(i = 0; i < lim; i++){
		x = fs->map[i];
		if(x == 0xff)
			continue;
		for(j = 0; j < 8; j++)
			if((x & (1<<j)) == 0){
				fs->map[i] = x|(1<<j);
				qunlock(fs);
				return i*8 + j;
			}
	}
	qunlock(fs);
	return -1;
}

/*
 *  see if we have a reasonable fat/root directory
 */
static int
fsinit(FS *fs)
{
	uchar buf[Blen+DIRLEN];
	Dir d;
	ulong x, bno;

	devtab[fs->c->type].stat(fs->c, buf);
	convM2D(buf, &d);
	fs->nblocks = d.length/Blen;
	if(fs->nblocks < 3)
		error("tinyfs medium too small");

	/* bitmap for block usage */
	x = (fs->nblocks + 8 - 1)/8;
	fs->map = malloc(x);
	memset(fs->map, 0x0, x);
	for(bno = fs->nblocks; bno < x*8; bno++)
		mapset(fs, bno);

	for(bno = 0; bno < fs->nblocks; bno++){
		n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
		if(n != Blen)
			break;
		if(checksum(buf) != 0)
			continue;
		switch(buf[0]){
		case Tdir:
			mapset(fs, bno);
			break;
		}
	}
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
