/*
 *  a pity the code isn't also tiny...
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

	Notapin=		0xffff,
	Notabno=		0xffff,

	Fcreating=	1,
	Frmonclose=	2,
};

/* representation of a Tdir on medium */
typedef struct Mdir Mdir;
struct Mdir {
	uchar	type;
	uchar	bno[2];
	uchar	pin[2];
	char	name[Nlen];
	char	pad[Blen - Nlen - 6];
	uchar	sum;
};

/* representation of a Tdata/Tend on medium */
typedef struct Mdata Mdata;
struct Mdata {
	uchar	type;
	uchar	bno[2];
	char	data[Dlen];
	uchar	sum;
};

typedef struct Tfile Tfile;
struct Tfile {
	Lock;
	int	r;
	char	name[NAMELEN];
	ushort	bno;
	ushort	dbno;
	ushort	pin;
	uchar	flag;
	ulong	length;
};

typedef struct Tfs Tfs;
struct Tfs {
	QLock;
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
	return s;
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
	if(p[0] != Tagdir)
		return 0;
	md = (Mdir*)p;
	x = GETS(md->bno);
	if(x >= fs->nblocks)
		return 0;
	return md;
}

static Mdata*
validdata(Tfs *fs, uchar *p, int *lenp)
{
	Mdata *md;
	ulong x;

	if(checksum(p) != 0)
		return 0;
	md = (Mdata*)p;
	switch(md->type){
	case Tagdata:
		x = GETS(md->bno);
		if(x >= fs->nblocks)
			return 0;
		if(lenp)
			*lenp = Dlen;
		break;
	case Tagend:
		x = GETS(md->bno);
		if(x > Dlen)
			return 0;
		if(lenp)
			*lenp = x;
		break;
	}
	return md;
}

static Mdata*
readdata(Tfs *fs, ulong bno, uchar *buf, int *lenp)
{
	if(bno >= fs->nblocks)
		return 0;
	if(devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno) != Blen)
		error(Eio);
	return validdata(fs, buf, lenp);
}

static void
writedata(Tfs *fs, ulong bno, ulong next, uchar *buf, int len, int last)
{
	Mdata md;

	if(bno >= fs->nblocks)
		error(Eio);
	if(len > Dlen)
		len = Dlen;
	if(len < 0)
		error(Eio);
	memset(&md, 0, sizeof(md));
	if(last){
		md.type = Tagend;
		PUTS(md.bno, len);
	} else {
		md.type = Tagdata;
		PUTS(md.bno, next);
	}
	memmove(md.data, buf, len);
	md.sum = 0 - checksum((uchar*)&md);
	
	if(devtab[fs->c->type].write(fs->c, &md, Blen, Blen*bno) != Blen)
		error(Eio);
}

static void
writedir(Tfs *fs, Tfile *f)
{
	Mdir *md;
	uchar buf[Blen];

	if(f->bno == Notabno)
		return;

	md = (Mdir*)buf;
	memset(buf, 0, Blen);
	md->type = Tagdir;
	strncpy(md->name, f->name, sizeof(md->name)-1);
	PUTS(md->bno, f->dbno);
	PUTS(md->pin, f->pin);
	md->sum = 0 - checksum(buf);

	if(devtab[fs->c->type].write(fs->c, buf, Blen, Blen*f->bno) != Blen)
		error(Eio);
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
	while(bno != bend && bno != Notabno){
		mapclr(fs, bno);
		n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
		if(n != Blen)
			break;
		md = validdata(fs, buf, 0);
		if(md == 0)
			break;
		if(md->type == Tagend)
			break;
		bno = GETS(md->bno);
	}

	/* change file type to free on medium */
	if(f->bno != Notabno){
		n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*f->bno);
		if(n != Blen)
			return;
		buf[0] = Tagfree;
		devtab[fs->c->type].write(fs->c, buf, Blen, Blen*f->bno);
	}

	/* forget we ever knew about it */
	memset(f, 0, sizeof(*f));
}

static void
expand(Tfs *fs)
{
	Tfile *f;

	fs->fsize += 8;
	f = smalloc(fs->fsize*sizeof(*f));

	memmove(f, fs->f, fs->nf*sizeof(f));
	free(fs->f);
	fs->f = f;
}

static Tfile*
newfile(Tfs *fs, char *name)
{
	int i;
	Tfile *f;

	/* find free entry in file table */
	f = 0;
	for(;;) {
		for(i = 0; i < fs->fsize; i++){
			f = &fs->f[i];
			if(f->name[0] == 0){
				strncpy(f->name, name, sizeof(f->name)-1);
				break;
			}
		}

		if(i < fs->fsize)
			break;

		expand(fs);
	}

	f->flag = Fcreating;
	f->dbno = Notabno;
	f->bno = mapalloc(fs);

	/* write directory block */
	if(waserror()){
		freefile(fs, f, Notabno);
		nexterror();
	}
	if(f->bno == Notabno)
		error("out of space");
	writedir(fs, f);
	poperror();
	
	return f;
}

/*
 *  Read the whole medium and build a file table and used
 *  block bitmap.  Inconsistent files are purged.  The medium
 *  had better be small or this could take a while.
 */
static void
fsinit(Tfs *fs)
{
	char dbuf[DIRLEN];
	Dir d;
	uchar buf[Blen];
	ulong x, bno;
	int n, done;
	Tfile *f;
	Mdir *mdir;
	Mdata *mdata;

	devtab[fs->c->type].stat(fs->c, dbuf);
	convM2D(dbuf, &d);
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

		mdir = validdir(fs, buf);
		if(mdir == 0)
			continue;

		if(fs->nf >= fs->fsize)
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
	for(f = fs->f; f < &(fs->f[fs->nf]); f++){
		bno = f->dbno;
		for(done = 0; !done;) {
			if(isalloced(fs, bno)){
				freefile(fs, f, bno);
				break;
			}
			n = devtab[fs->c->type].read(fs->c, buf, Blen, Blen*bno);
			if(n != Blen){
				freefile(fs, f, bno);
				break;
			}
			mdata = validdata(fs, buf, 0);
			if(mdata == 0){
				freefile(fs, f, bno);
				break;
			}
			mapset(fs, bno);
			switch(mdata->type){
			case Tagdata:
				bno = GETS(mdata->bno);
				f->length += Dlen;
				break;
			case Tagend:
				f->length += GETS(mdata->bno);
				done = 1;
				break;
			}
			if(done)
				f->flag &= ~Fcreating;
		}
	}
}

/*
 *  single directory
 */
static int
tinyfsgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	Tfs *fs;
	Tfile *f;
	Qid qid;

	USED(ntab, tab);

	fs = &tinyfs.fs[c->dev];
	if(i >= fs->nf)
		return -1;
	f = &fs->f[i];
	qid.path = i;
	qid.vers = 0;
	devdir(c, qid, f->name, f->length, eve, 0664, dp);
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

	cc = namec(spec, Aopen, ORDWR, 0);
	if(waserror()){
		close(cc);
		qunlock(&tinyfs);
		nexterror();
	}

	qlock(&tinyfs);
	fs = 0;
	for(i = 0; i < tinyfs.nfs; i++){
		fs = &tinyfs.fs[i];
		if(fs && eqchan(cc, fs->c, 0))
			break;
	}
	if(i < tinyfs.nfs){
		qlock(fs);
		fs->r++;
		qunlock(fs);
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
	Tfs *fs;

	fs = &tinyfs.fs[c->dev];

	qlock(fs);
	fs->r++;
	qunlock(fs);

	return devclone(c, nc);
}

int
tinyfswalk(Chan *c, char *name)
{
	int n;
	Tfs *fs;

	fs = &tinyfs.fs[c->dev];

	qlock(fs);
	n = devwalk(c, name, 0, 0, tinyfsgen);
	if(n != 0 && c->qid.path != CHDIR){
		fs = &tinyfs.fs[c->dev];
		fs->f[c->qid.path].r++;
	}
	qunlock(fs);
	return n;
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

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		qlock(fs);
		if(omode == (OTRUNC|ORDWR)){
			f = newfile(fs, fs->f[c->qid.path].name);
			c->qid.path = f - fs->f;
		} else if(omode != OREAD){
			qunlock(fs);
			error(Eperm);
		}
		qunlock(fs);
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

	qlock(fs);
	f = newfile(fs, name);
	qunlock(fs);

	c->qid.path = f - fs->f;
	c->qid.vers = 0;
	c->mode = openmode(omode);
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
	int i;

	fs = &tinyfs.fs[c->dev];

	qlock(fs);

	/* dereference file and remove old versions */
	if(c->qid.path != CHDIR){
		f = &fs->f[c->qid.path];
		f->r--;
		if(f->r == 0){
			if(f->flag & Fcreating){
				/* remove all other files with this name */
				for(i = 0; i < fs->fsize; i++){
					nf = &fs->f[i];
					if(f == nf)
						continue;
					if(strcmp(nf->name, f->name) == 0){
						if(nf->r)
							nf->flag |= Frmonclose;
						else
							freefile(fs, nf, Notabno);
					}
				}
				f->flag &= ~(Frmonclose|Fcreating);
			}
			if(f->flag & Frmonclose)
				freefile(fs, f, Notabno);
		}
	}

	/* dereference fs and remove on zero refs */
	fs->r--;
	qunlock(fs);
	qlock(&tinyfs);
	if(fs->r == 0){
		for(l = &fs->l; *l;){
			if(*l == fs){
				*l = fs->next;
				break;
			}
			l = &(*l)->next;
		}
		free(fs->f);
		free(fs->map);
		close(fs->c);
		memset(fs, 0, sizeof(*fs));
	}
	qunlock(&tinyfs);
}

long
tinyfsread(Chan *c, void *a, long n, ulong offset)
{
	Tfs *fs;
	Tfile *f;
	int sofar, i;
	ulong bno;
	Mdata *md;
	uchar buf[Blen];
	uchar *p = a;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, tinyfsgen);

	fs = tinyfs.fs[c->dev];
	f = &fs->f[c->qid.path];
	if(offset >= f->length)
		return 0;
	if(n + offset >= f->length)
		n = f->length - offset;

	/* walk to starting data block */
	bno = f->dbno;
	for(sofar = 0; sofar + Blen < offset; sofar += Blen){
		md = readdata(fs, bno, buf, 0);
		bno = GETS(md->bno);
	}

	/* read data */
	offset = offset%Blen;
	for(sofar = 0; sofar < n; sofar += i){
		md = readdata(fs, bno, buf, &i);
		i -= offset;
		if(i > n)
			i = n;
		if(i < 0)
			break;
		memmove(p, md->data, i);
		p += i;
		bno = GETS(md->bno);
		offset = 0;
	}

	return sofar;
}

Block*
tinyfsbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
tinyfswrite(Chan *c, char *a, long n, ulong offset)
{
	Tfs *fs;
	Tfile *f;
	int sofar, i, x;
	ulong bno, tbno;
	Mdata *md;
	uchar buf[Blen];
	uchar *p = a;

	if(c->qid.path & CHDIR)
		error(Eperm);

	if(n == 0)
		return 0;

	fs = tinyfs.fs[c->dev];
	f = &fs->f[c->qid.path];

	/* files are append only, anything else is illegal */
	if(offset != f->length)
		error("append only");

	qlock(fs);
	if(waserror()){
		f->flag |= Frmonclose;
		qunlock(fs);
		nexterror();
	}

	/* walk to last data block */
	bno = f->dbno;
	for(sofar = 0; sofar + Blen < offset; sofar += Blen){
		md = readdata(fs, bno, buf, 0);
		if(md->type == Tagend)
			break;
		bno = GETS(md->bno);
	}

	sofar = 0;
	i = offset%Dlen;
	if(i){
		x = n;
		if(i + x > Dlen)
			x = Dlen - i;
		memmove(md->data + i, p, sofar);
		f->length += x;
		sofar += x;
	}

	while(x = n - sofar) {
		tbno = mapalloc(fs);
		if(f->length == 0){
			f->dbno = tbno;
			writedir(fs, f);
		} else {
			writedata(fs, bno, tbno, md->data, Dlen, 0);
		}
		if(x > Dlen)
			x = Dlen;
		memmove(md->data, p + sofar, x);
		sofar += x;
		f->length += x;
		bno = tbno;
	}

	i = f->length%Dlen;
	if(i == 0)
		i = Dlen;
	writedata(fs, bno, tbno, md->data, i, 1);

	poperror();
	qunlock(fs);

	return sofar;
}

long
tinyfsbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}
