/*
 *  a pity the code isn't also tiny...
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"


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
	uchar	data[Dlen];
	uchar	sum;
};

typedef struct Tfile Tfile;
struct Tfile {
	int	r;
	char	name[NAMELEN];
	ushort	bno;
	ushort	dbno;
	ushort	pin;
	uchar	flag;
	ulong	length;

	/* hint to avoid egregious reading */
	ushort	fbno;
	ulong	finger;
};

typedef struct Tfs Tfs;
struct Tfs {
	QLock	ql;
	int	r;
	Chan	*c;
	uchar	*map;
	int	nblocks;
	Tfile	*f;
	int	nf;
	int	fsize;
};

struct {
	Tfs	fs[Maxfs];
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

	lim = (fs->nblocks + 8 - 1)/8;
	for(i = 0; i < lim; i++){
		x = fs->map[i];
		if(x == 0xff)
			continue;
		for(j = 0; j < 8; j++)
			if((x & (1<<j)) == 0){
				fs->map[i] = x|(1<<j);
				return i*8 + j;
			}
	}

	return Notabno;
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
	if(devtab[fs->c->type]->read(fs->c, buf, Blen, Blen*bno) != Blen)
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
	
	if(devtab[fs->c->type]->write(fs->c, &md, Blen, Blen*bno) != Blen)
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

	if(devtab[fs->c->type]->write(fs->c, buf, Blen, Blen*f->bno) != Blen)
		error(Eio);
}

static void
freeblocks(Tfs *fs, ulong bno, ulong bend)
{
	uchar buf[Blen];
	Mdata *md;

	if(waserror())
		return;

	while(bno != bend && bno != Notabno){
		mapclr(fs, bno);
		if(devtab[fs->c->type]->read(fs->c, buf, Blen, Blen*bno) != Blen)
			break;
		md = validdata(fs, buf, 0);
		if(md == 0)
			break;
		if(md->type == Tagend)
			break;
		bno = GETS(md->bno);
	}

	poperror();
}

static void
freefile(Tfs *fs, Tfile *f, ulong bend)
{
	uchar buf[Blen];

	/* remove blocks from map */
	freeblocks(fs, f->dbno, bend);

	/* change file type to free on medium */
	if(f->bno != Notabno){
		if(devtab[fs->c->type]->read(fs->c, buf, Blen, Blen*f->bno) != Blen)
			return;
		buf[0] = Tagfree;
		devtab[fs->c->type]->write(fs->c, buf, Blen, Blen*f->bno);
		mapclr(fs, f->bno);
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

	if(fs->f){
		memmove(f, fs->f, fs->nf*sizeof(*f));
		free(fs->f);
	}
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

		if(i < fs->fsize){
			if(i >= fs->nf)
				fs->nf = i+1;
			break;
		}

		expand(fs);
	}

	f->flag = Fcreating;
	f->dbno = Notabno;
	f->bno = mapalloc(fs);
	f->fbno = Notabno;
	f->r = 1;

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

	devtab[fs->c->type]->stat(fs->c, dbuf);
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
		n = devtab[fs->c->type]->read(fs->c, buf, Blen, Blen*bno);
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
			n = devtab[fs->c->type]->read(fs->c, buf, Blen, Blen*bno);
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
	if(f->name[0] == 0)
		return 0;
	qid.path = i;
	qid.vers = 0;
	devdir(c, qid, f->name, f->length, eve, 0664, dp);
	return 1;
}

static void
tinyfsreset(void)
{
	if(Nlen > NAMELEN)
		panic("tinyfsreset");
}

static Chan*
tinyfsattach(char *spec)
{
	Tfs *fs;
	Chan *c, *cc;
	int i;
	char *p;

	p = 0;
	if(strncmp(spec, "hd0", 3) == 0)
		p = "/dev/hd0nvram";
	else if(strncmp(spec, "hd1", 3) == 0)
		p = "/dev/hd1nvram";
	else if(strncmp(spec, "sd0", 3) == 0)
		p = "/dev/sd0nvram";
	else if(strncmp(spec, "sd1", 3) == 0)
		p = "/dev/sd1nvram";
	else
		error("bad spec");

	cc = namec(p, Aopen, ORDWR, 0);
	if(waserror()){
		cclose(cc);
		nexterror();
	}

	fs = 0;
	for(i = 0; i < Maxfs; i++){
		fs = &tinyfs.fs[i];
		qlock(&fs->ql);
		if(fs->r && eqchan(cc, fs->c, 0))
			break;
		qunlock(&fs->ql);
	}
	if(i < Maxfs){
		fs->r++;
		qunlock(&fs->ql);
		cclose(cc);
	} else {
		for(fs = tinyfs.fs; fs < &tinyfs.fs[Maxfs]; fs++){
			qlock(&fs->ql);
			if(fs->r == 0)
				break;
			qunlock(&fs->ql);
		}
		if(fs == &tinyfs.fs[Maxfs])
			error("too many tinyfs's");
		fs->c = cc;
		fs->r = 1;
		fs->f = 0;
		fs->nf = 0;
		fs->fsize = 0;
		fsinit(fs);
		qunlock(&fs->ql);
	}
	poperror();

	c = devattach('U', spec);
	c->dev = fs - tinyfs.fs;
	c->qid.path = CHDIR;
	c->qid.vers = 0;

	return c;
}

static Chan*
tinyfsclone(Chan *c, Chan *nc)
{
	Tfs *fs;

	fs = &tinyfs.fs[c->dev];

	qlock(&fs->ql);
	fs->r++;
	qunlock(&fs->ql);

	return devclone(c, nc);
}

static int
tinyfswalk(Chan *c, char *name)
{
	int n;
	Tfs *fs;

	fs = &tinyfs.fs[c->dev];

	qlock(&fs->ql);
	n = devwalk(c, name, 0, 0, tinyfsgen);
	if(n != 0 && c->qid.path != CHDIR){
		fs = &tinyfs.fs[c->dev];
		fs->f[c->qid.path].r++;
	}
	qunlock(&fs->ql);
	return n;
}

static void
tinyfsstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, tinyfsgen);
}

static Chan*
tinyfsopen(Chan *c, int omode)
{
	Tfs *fs;
	Tfile *f;

	fs = &tinyfs.fs[c->dev];

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		qlock(&fs->ql);
		if(waserror()){
			qunlock(&fs->ql);
			nexterror();
		}
		switch(omode){
		case OTRUNC|ORDWR:
		case OTRUNC|OWRITE:
			f = newfile(fs, fs->f[c->qid.path].name);
			fs->f[c->qid.path].r--;
			c->qid.path = f - fs->f;
			break;
		case OREAD:
			break;
		default:
			error(Eperm);
		}
		qunlock(&fs->ql);
		poperror();
	}

	return devopen(c, omode, 0, 0, tinyfsgen);
}

static void
tinyfscreate(Chan *c, char *name, int omode, ulong perm)
{
	Tfs *fs;
	Tfile *f;

	USED(perm);

	fs = &tinyfs.fs[c->dev];

	qlock(&fs->ql);
	if(waserror()){
		qunlock(&fs->ql);
		nexterror();
	}
	f = newfile(fs, name);
	qunlock(&fs->ql);
	poperror();

	c->qid.path = f - fs->f;
	c->qid.vers = 0;
	c->mode = openmode(omode);
}

static void
tinyfsremove(Chan *c)
{
	Tfs *fs;
	Tfile *f;

	if(c->qid.path == CHDIR)
		error(Eperm);
	fs = &tinyfs.fs[c->dev];
	f = &fs->f[c->qid.path];
	qlock(&fs->ql);
	freefile(fs, f, Notabno);
	qunlock(&fs->ql);
}

static void
tinyfsclose(Chan *c)
{
	Tfs *fs;
	Tfile *f, *nf;
	int i;

	fs = &tinyfs.fs[c->dev];

	qlock(&fs->ql);

	/* dereference file and remove old versions */
	if(!waserror()){
		if(c->qid.path != CHDIR){
			f = &fs->f[c->qid.path];
			f->r--;
			if(f->r == 0){
				if(f->flag & Frmonclose)
					freefile(fs, f, Notabno);
				else if(f->flag & Fcreating){
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
					f->flag &= ~Fcreating;
				}
			}
		}
		poperror();
	}

	/* dereference fs and remove on zero refs */
	fs->r--;
	if(fs->r == 0){
		if(fs->f)
			free(fs->f);
		fs->f = 0;
		fs->nf = 0;
		fs->fsize = 0;
		if(fs->map)
			free(fs->map);
		fs->map = 0;
		cclose(fs->c);
		fs->c = 0;
	}
	qunlock(&fs->ql);
}

static long
tinyfsread(Chan *c, void *a, long n, ulong offset)
{
	Tfs *fs;
	Tfile *f;
	int sofar, i, off;
	ulong bno;
	Mdata *md;
	uchar buf[Blen];
	uchar *p = a;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, tinyfsgen);

	fs = &tinyfs.fs[c->dev];
	f = &fs->f[c->qid.path];
	if(offset >= f->length)
		return 0;

	qlock(&fs->ql);
	if(waserror()){
		qunlock(&fs->ql);
		nexterror();
	}
	if(n + offset >= f->length)
		n = f->length - offset;

	/* walk to starting data block */
	if(f->finger < offset && f->fbno != Notabno){
		sofar = f->finger;
		bno = f->fbno;
	} else {
		sofar = 0;
		bno = f->dbno;
	}
	for(; sofar + Dlen < offset; sofar += Dlen){
		md = readdata(fs, bno, buf, 0);
		if(md == 0)
			error(Eio);
		bno = GETS(md->bno);
	}

	/* read data */
	off = offset%Dlen;
	offset -= off;
	for(sofar = 0; sofar < n; sofar += i){
		md = readdata(fs, bno, buf, &i);
		if(md == 0)
			error(Eio);

		/* update finger for successful read */
		f->finger = offset + sofar;
		f->fbno = bno;

		i -= off;
		if(i > n)
			i = n;
		memmove(p, md->data, i);
		p += i;
		bno = GETS(md->bno);
		off = 0;
	}
	qunlock(&fs->ql);
	poperror();

	return sofar;
}

/*
 *  if we get a write error in this routine, blocks will
 *  be lost.  They should be recovered next fsinit.
 */
static long
tinyfswrite(Chan *c, void *a, long n, ulong offset)
{
	Tfs *fs;
	Tfile *f;
	int last, next, i, off, finger;
	ulong bno, dbno, fbno;
	Mdata *md;
	uchar buf[Blen];
	uchar *p = a;

	if(c->qid.path & CHDIR)
		error(Eperm);

	if(n == 0)
		return 0;

	fs = &tinyfs.fs[c->dev];
	f = &fs->f[c->qid.path];

	/* files are append only, anything else is illegal */
	if(offset != f->length)
		error("append only");

	qlock(&fs->ql);
	dbno = Notabno;
	if(waserror()){
		freeblocks(fs, dbno, Notabno);
		qunlock(&fs->ql);
		nexterror();
	}

	/* write blocks backwards */
	p += n;
	last = offset + n;
	off = offset;
	fbno = Notabno;
	finger = 0;
	for(next = (last/Dlen)*Dlen; next >= off; next -= Dlen){
		bno = mapalloc(fs);
		if(bno == Notabno){
			error("out of space");
		}
		i = last - next;
		p -= i;
		if(last == n+offset){
			writedata(fs, bno, dbno, p, i, 1);
			finger = next;	/* remember for later */
			fbno = bno;
		} else
			writedata(fs, bno, dbno, p, i, 0);
		dbno = bno;
		last = next;
	}

	/* walk to last data block */
	md = (Mdata*)buf;
	if(f->finger < offset && f->fbno != Notabno){
		next = f->finger;
		bno = f->fbno;
	} else {
		next = 0;
		bno = f->dbno;
	}
	for(; next < offset; next += Dlen){
		md = readdata(fs, bno, buf, 0);
		if(md == 0)
			error(Eio);
		if(md->type == Tagend)
			break;
		bno = GETS(md->bno);
	}

	/* point to new blocks */
	if(offset == 0){
		f->dbno = dbno;
		writedir(fs, f);
	} else {
		i = last - offset;
		next = offset%Dlen;
		if(i > 0){
			p -= i;
			memmove(md->data + next, p, i);
		}
		writedata(fs, bno, dbno, md->data, i+next, last == n+offset);
	}
	f->length += n;

	/* update finger */
	if(fbno != Notabno){
		f->finger = finger;
		f->fbno =  fbno;
	}
	poperror();
	qunlock(&fs->ql);

	return n;
}

Dev tinyfsdevtab = {
	tinyfsreset,
	devinit,
	tinyfsattach,
	tinyfsclone,
	tinyfswalk,
	tinyfsstat,
	tinyfsopen,
	tinyfscreate,
	tinyfsclose,
	tinyfsread,
	devbread,
	tinyfswrite,
	devbwrite,
	tinyfsremove,
	devwstat,
};
