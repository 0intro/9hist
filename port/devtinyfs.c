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

	Maxfs=		10,	/* max file systems */

	Blen=		48,	/* block length */
	Nlen=		28,	/* name length */
	Dlen=		Blen - 4,

	Tagdir=		'd',
	Tagdata=	'D',
	Tagend=		'e',
	Tagfree=	'f',

	Nopin=		0xffff;
};

/* medium representation of a Tdir */
typedef struct Mdir Mdir;
struct Mdir {
	uchar	type;
	uchar	bno[2];
	uchar	pin[2];
	char	name[Nlen];
	char	pad[Blen - Nlen - 6];
	uchar	sum;
};

/* medium representation of a Tdata/Tend */
typedef struct Mdata Mdata;
struct Mdata {
	uchar	type;
	uchar	bno[2];
	char	data[Dlen];
	uchar	sum;
};

typedef struct Tfile Tfile;
struct Tfile {
	char	name[NAMELEN];
	ushort	bno;
	ushort	dbno;
	ushort	pin;
	ulong	length;
};

typedef struct Tfs Tfs;
struct Tfs {
	Lock;
	int	r;
	Chan	*c;
	uchar	*map;
	int	nblocks;
	Tfile	*f;
	int	nf;
	int	fsize;
};

struct {
	QLock;
	Tfs	fs[Maxfs];
	short	nfs;
} tinyfs;

#define GETS(x) ((x)[0]|((x)[1]<<8))
#define PUTS(x, v) {(x)[0] = (v);(x)[1] = ((v)>>8);}

#define GETL(x) (GETS(x)|(GETS(x+2)<<16))
#define PUTL(x, v) {PUTS(x, v);PUTS(x+2, (v)>>16)};

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
mapclr(Tfs *fs, ulong bno)
{
	fs->map[bno>>3] &= ~(1<<(bno&7));
}

static void
mapset(Tfs *fs, ulong bno)
{
	fs->map[bno>>3] |= 1<<(bno&7);
}

static int
isalloced(Tfs *fs, ulong bno)
{
	return fs->map[bno>>3] & (1<<(bno&7));
}

static int
mapalloc(Tfs *fs)
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

static Mdir*
validdir(Tfs *fs, uchar *p)
{
	Mdir *md;
	ulong x;

	if(checksum(p) != 0)
		return 0;
	if(buf[0] != Tagdir)
		return 0;
	md = (Mdir*)p;
	x = GETS(md->bno);
	if(x >= fs->nblocks)
		return 0;
	return md;
}

static Mdata*
validdata(Tfs *fs, uchar *p)
{
	Mdata *md;
	ulong x;

	if(checksum(p) != 0)
		return 0;
	md = (Mdir*)p;
	switch(buf[0]){
	case Tagdata:
		x = GETS(md->bno);
		if(x >= fs->nblocks)
			return 0;
		break;
	case Tagend:
		x = GETS(md->bno);
		if(x > Blen - 4)
			return 0;
		break;
	}
	return md;
}

static void
freefile(Tfs *fs, Tfile *f, ulong bend)
{
	uchar buf[Blen];
	ulong bno;
	int n;
	Mdata *md;

	/* remove blocks from map */
	bno = f->dbno;
	while(bend != bno){
		mapclr(fs, bno);
		n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
		if(n != Blen)
			break;
		md = validdata(buf);
		if(md == 0)
			break;
		if(md->type == Tagend)
			break;
		bno = GETS(md->bno);
	}

	/* change file type to free on medium */
	n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*f->bno);
	if(n != Blen)
		return;
	buf[0] = Tagfree;
	devtab[fs->c->type].write(fs->c, buf, Blen, Blen*f->bno);

	/* forget we ever knew about it */
	memset(f, 0, sizeof(*f));
}

static void
expand(Tfs *fs)
{
	Tfile *f;

	fs->fsize += 8;
	f = smalloc(fs->fsize*sizeof(*f));

	lock(fs);
	memmove(f, fs->f, fs->nf*sizoef(f));
	free(fs->f);
	fs->f = f;
	unlock(fs);
}

/*
 *  see if we have a reasonable fat/root directory
 */
static void
fsinit(Tfs *fs)
{
	uchar buf[Blen+DIRLEN];
	Dir d;
	ulong x, bno;
	int n;
	Tfile *f;
	Mdir *mdir;
	Mdata *mdat;

	devtab[fs->c->type].stat(fs->c, buf);
	convM2D(buf, &d);
	fs->nblocks = d.length/Blen;
	if(fs->nblocks < 3)
		error("tinyfs medium too small");

	/* bitmap for block usage */
	x = (fs->nblocks + 8 - 1)/8;
	fs->map = smalloc(x);
	memset(fs->map, 0x0, x);
	for(bno = fs->nblocks; bno < x*8; bno++)
		mapset(fs, bno);

	/* find files */
	for(bno = 0; bno < fs->nblocks; bno++){
		n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
		if(n != Blen)
			break;

		mdir = validdir(buf);
		if(mdir == 0)
			continue;

		if(fs->nfs <= fs->fsize)
			expand(fs);
		f = &fs->f[fs->nf++];

		x = GETS(mdir->bno);
		mapset(fs, bno);
		strncpy(f->name, mdir->name, sizeof(f->name));
		f->pin = GETS(mdir->pin);
		f->bno = bno;
		f->dbno = x;
	}

	/* follow files */
	for(f = fs->f; f; f = f->next){
		bno = fs->dbno;
		for(;;) {
			if(isalloced(fs, bno)){
				freefile(f, bno);
				break;
			}
			n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
			if(n != Blen){
				freefile(fs, f, bno);
				break;
			}
			mdata = validdata(fs, buf);
			if(mdata == 0){
				freefile(fs, f, bno);
				break;
			}
			mapset(fs, bno);
			switch(mdata->type){
			case Tagdata:
				bno = GETS(mdata->bno);
				f->len += Dlen;
				break;
			case Tagend:
				f->len += GETS(mdata->bno);
				break;
			}
		}
	}
}

static int
tinyfsgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	Tfs *fs;
	Tfile *f;
	Qid qid;

	fs = &tinyfs.fs[c->dev];
	if(i >= fs->nf)
		return -1;
	f = &fs->f[i];
	qid.path = i;
	qid.vers = 0;
	devdir(c, qid, f->name, f->length, eve, f->pin==Nopin?0444:0666, dp);
	return 1;
}

void
tinyfsreset(void)
{
	if(Nlen > NAMELEN)
		panic("tinyfsreset");
}

void
tinyfsinit(void)
{
}

Chan *
tinyfsattach(char *spec)
{
	Tfs *fs;
	Chan *c, *cc;
	int i;

	cc = namec((char*)arg[0], Aopen, arg[1], 0);
	if(waserror()){
		close(cc);
		qunlock(&tinyfs);
		nexterror();
	}

	qlock(&tinyfs);
	for(i = 0; i < tinyfs.nfs; i++){
		fs = &tinyfs.fs[i];
		if(fs && eqchan(c, fs->c))
			break;
	}
	if(i < tinyfs.nfs){
		lock(fs);
		fs->r++;
		unlock(fs);
		close(cc);
	} else {
		if(tinyfs.nfs >= Maxfs)
			error("too many tinyfs's");
		fs = &tinyfs.fs[tinyfs.nfs];
		memset(fs, 0, sizeof(*fs));
		fs->c = cc;
		fs->r = 1;
		fsinit(fs);
		tinyfs.nfs++;
	}
	qunlock(&tinyfs);
	poperror();

	c = devattach('U', spec);
	c->dev = fs - tinyfs.fs;
	c->qid.path = CHDIR;
	c->qid.vers = 0;

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
	return devwalk(c, name, 0, 0, tinyfsgen);
}

void
tinyfsstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, tinyfsgen);
}

Chan *
tinyfsopen(Chan *c, int omode)
{
	Tfs *fs;
	Tfile *f;

	fs = &tinyfs.fs[c->dev];

	if(c->path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		lock(fs);
		f = fs->f[c->path];
		unlock(fs);

		if(f->pin == Nopin){
			if(omode != OREAD)
				error(Eperm);
		} else {
			if(omode != ORDWR)
				error(Eperm);
		}
	}

	return devopen(c, omode, 0, 0, tinyfsgen);
}

void
tinyfscreate(Chan *c, char *name, int omode, ulong perm)
{
	Tfs *fs;
	Tfile *f;

	if(perm & CHDIR)
		error("directory creation illegal");

	fs = &tinyfs.fs[c->dev];
	for(;;) {
		lock(fs);
		for(i = 0; i < fs->fsize; i++){
			f = &fs->f[i];
			if(f->name[0] == 0){
				strncpy(f->name, name, sizeof(f->name)-1);
				break;
			}
		}
		if(i < fs->fsize)
			break;

		unlock(fs);
		expand(fs);
	}
	unlock(fs);

	c->qid.path = f - fs->f;
	c->qid.vers = 1;		/* creating */
	c->mode = openmode(omode);
	c->flag |= COPEN;
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
	Tfs *fs, **l;
	Tfile *f, *nf;

	fs = c->aux;
	lock(fs);
	fs->ref--;
	unlock(fs);

	if(fs->ref)
		return;

	qlock(&tinyfs);
	lock(fs);
	if(fs->ref == 0){
		for(l = &fs->l; *l;){
			if(*l == fs){
				*l = fs->next;
				break;
			}
			l = &(*l)->next;
		}
		for(f = fs->f; f; f = nf){
			nf = f->next;
			free(f);
		}
		free(fs->map);
		close(fs->c);
		free(fs);
	}
	unlock(fs);
	qunlock(&tinyfs);
}

long
tinyfsread(Chan *c, void *a, long n, ulong offset)
{
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, tinyfstab, Ntinyfstab, tinyfsgen);
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
