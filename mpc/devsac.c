#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

/*
 * Rather than reading /adm/users, which is a lot of work for
 * a toy program, we assume all groups have the form
 *	NNN:user:user:
 * meaning that each user is the leader of his own group.
 */

enum
{
	OPERM	= 0x3,		/* mask of all permission types in open mode */
	Nram	= 512,
};

typedef struct SacPath SacPath;
typedef struct Sac Sac;
typedef struct SacHeader SacHeader;
typedef struct SacDir SacDir;

enum {
	Magic = 0x5acf5,
};

struct SacDir
{
	char	name[NAMELEN];
	char	uid[NAMELEN];
	char	gid[NAMELEN];
	uchar	qid[4];
	uchar	mode[4];
	uchar	atime[4];
	uchar	mtime[4];
	uchar	length[8];
	uchar	blocks[8];
};

struct SacHeader
{
	uchar	magic[4];
	uchar	length[8];
	uchar	blocksize[4];
	uchar	md5[16];
};


struct Sac
{
	SacDir;
	SacPath *path;
};

struct SacPath
{
	Ref;
	SacPath *up;
	vlong blocks;
	int entry;
};

enum
{
	Pexec =		1,
	Pwrite = 	2,
	Pread = 	4,
	Pother = 	1,
	Pgroup = 	8,
	Powner =	64,
};

uchar *data = SACMEM;
int blocksize;
Sac root;

void	sacstat(SacDir*, char*);
void	io(void);
void	usage(void);
ulong	getl(void *p);
vlong	getv(void *p);
void	init(char*);
Sac	*saccpy(Sac *s);
Sac *saclookup(Sac *s, char *name);
int sacdirread(Sac *s, char *p, long off, long cnt);
void loadblock(void *buf, uchar *offset, int blocksize);
void sacfree(Sac*);

void
devinit(void)
{
	SacHeader *hdr;
	hdr = (SacHeader*)data;
	if(getl(hdr->magic) != Magic) {
print("devsac: bad magic");
		return;
	}
	blocksize = getl(hdr->blocksize);
	root.SacDir = *(SacDir*)(data + sizeof(SacHeader));
}

static Chan*
sacattach(char* spec)
{
	Chan *c;
	int dev;

	dev = atoi(spec);
	if(dev != 0)
		error("bad specification");

	// check if init found sac file system in memory
	if(blocksize == 0)
		error("devsac: bad magic");

	c = devattach('C', spec);
	c->qid = (Qid){getl(root.qid), 0};
	c->dev = dev;
	c->aux = saccpy(&root);
	return c;
}

Chan*
sacclone(Chan *c, Chan *nc)
{
	nc = devclone(c, nc);
	nc->aux = saccpy(c->aux);
	return nc;
}

int
devwalk(Chan *c, char *name, Dirtab *tab, int ntab, Devgen *gen)
{
	Sac *sac;

	isdir(c);
	sac = c->aux;
	if(strcmp(name, ".") == 0)
		return 1;
	sac = saclookup(sac, name);
	if(sac == nil) {
		strncpy(up->error, Enonexist, NAMELEN);
		return 0;
	}
	c->aux = sac;
	c->qid = (Qid){getl(sac->qid), 0};
	op = c->path;
	c->path = ptenter(&syspt, op, name);
	decref(op);
	return 1;
}

char *
ropen(Fid *f)
{
	int mode, trunc;

	if(f->open)
		return Eisopen;
	if(f->busy == 0)
		return Enotexist;
	mode = rhdr.mode;
	if(f->qid.path & CHDIR){
		if(mode != OREAD)
			return Eperm;
		thdr.qid = f->qid;
		return 0;
	}
	if(mode & ORCLOSE)
		return Erdonly;
	trunc = mode & OTRUNC;
	mode &= OPERM;
	if(mode==OWRITE || mode==ORDWR || trunc)
		return Erdonly;
	if(mode==OREAD)
		if(!perm(f, f->sac, Pread))
			return Eperm;
	if(mode==OEXEC)
		if(!perm(f, f->sac, Pexec))
			return Eperm;
	thdr.qid = f->qid;
	f->open = 1;
	return 0;
}

char *
rcreate(Fid *f)
{
	if(f->open)
		return Eisopen;
	if(f->busy == 0)
		return Enotexist;
	return Erdonly;
}

char*
rread(Fid *f)
{
	Sac *sac;
	char *buf, *buf2;
	long off;
	int n, cnt, i, j;
	uchar *blocks;
	vlong length;

	if(f->busy == 0)
		return Enotexist;
	sac = f->sac;
	thdr.count = 0;
	off = rhdr.offset;
	buf = thdr.data;
	cnt = rhdr.count;
	if(f->qid.path & CHDIR){
		cnt = (rhdr.count/DIRLEN)*DIRLEN;
		if(off%DIRLEN)
			return "i/o error";
		thdr.count = sacdirread(sac, buf, off, cnt);
		return 0;
	}
	length = getv(sac->length);
	if(off >= length) {
		rhdr.count = 0;
		return 0;
	}
	if(cnt > length-off)
		cnt = length-off;
	thdr.count = cnt;
	if(cnt == 0)
		return 0;
	blocks = data + getv(sac->blocks);
	buf2 = malloc(blocksize);
	while(cnt > 0) {
		i = off/blocksize;
		loadblock(buf2, blocks+i*8, blocksize);
		j = off-i*blocksize;
		n = blocksize-j;
		if(n > cnt)
			n = cnt;
		memmove(buf, buf2+j, n);
		cnt -= n;
		off += n;
	}
	free(buf2);
	return 0;
}

char*
sacwrite(Fid *f)
{
	if(f->busy == 0)
		return Enotexist;
	return Erdonly;
}

char *
rclunk(Fid *f)
{
	f->busy = 0;
	f->open = 0;
	free(f->user);
	sacfree(f->sac);
	return 0;
}

char *
rremove(Fid *f)
{
	f->busy = 0;
	f->open = 0;
	free(f->user);
	sacfree(f->sac);
	return Erdonly;
}

char *
rstat(Fid *f)
{
	if(f->busy == 0)
		return Enotexist;
	sacstat(f->sac, thdr.stat);
	return 0;
}

char *
rwstat(Fid *f)
{
	if(f->busy == 0)
		return Enotexist;
	return Erdonly;
}

Sac*
saccpy(Sac *s)
{
	Sac *ss;
	
	ss = malloc(sizeof(Sac));
	*ss = *s;
	if(ss->path)
		incref(ss->path);
	return ss;
}

SacPath *
sacpathalloc(SacPath *p, vlong blocks, int entry)
{
	SacPath *pp = malloc(sizeof(SacPath));
	pp->ref = 1;
	pp->blocks = blocks;
	pp->entry = entry;
	pp->up = p;
	return pp;
}

static void
sacpathfree(SacPath *p)
{
	if(p == nil)
		return;
	if(decref(p) > 0)
		return;
	sacpathfree(p->up);
	free(p);
}


void
sacfree(Sac *s)
{
	sacpathfree(s->path);
	free(s);
}

void
sacstat(SacDir *s, char *buf)
{
	Dir dir;

	memmove(dir.name, s->name, NAMELEN);
	dir.qid = (Qid){getl(s->qid), 0};
	dir.mode = getl(s->mode);
	dir.length = getv(s->length);
	if(dir.mode &CHDIR)
		dir.length *= DIRLEN;
	strcpy(dir.uid, s->uid);
	strcpy(dir.gid, s->gid);
	dir.atime = getl(s->atime);
	dir.mtime = getl(s->mtime);
	convD2M(&dir, buf);
}

void
loadblock(void *buf, uchar *offset, int blocksize)
{
	vlong block, n;

	block = getv(offset);
	if(block < 0) {
		block = -block;
		n = getv(offset+8);
		if(n < 0)
			n = -n;
		n -= block;
//fprint(2, "blocksize = %d, block = %lld n = %lld\n", blocksize, block, n);
		if(unsac(buf, data+block, blocksize, n)<0)
			panic("unsac failed!");
	} else {
		memmove(buf, data+block, blocksize);
	}
}

Sac*
sacparent(Sac *s)
{
	uchar *blocks;
	SacDir *buf;
	int per, i;
	SacPath *p;

	p = s->path;
	if(p == nil || p->up == nil) {
		pathfree(p);
		*s = root;
		return s;
	}
	p = p->up;

//fprint(2, "sacparent = %lld %d\n", p->blocks, p->entry);
	blocks = data + p->blocks;
	per = blocksize/sizeof(SacDir);
	i = p->entry/per;
	buf = malloc(per*sizeof(SacDir));
	loadblock(buf, blocks + i*8, per*sizeof(SacDir));
	s->SacDir = buf[p->entry-i*per];
//fprint(2, "sacparent = %s\n", s->name);
	free(buf);
	incref(p);
	pathfree(s->path);
	s->path = p;
	return s;
}

int
sacdirread(Sac *s, char *p, long off, long cnt)
{
	uchar *blocks;
	SacDir *buf;
	int iblock, per, i, j, ndir;

	blocks = data + getv(s->blocks);
	per = blocksize/sizeof(SacDir);
	ndir = getv(s->length);
	off /= DIRLEN;
	cnt /= DIRLEN;
	if(off >= ndir)
		return 0;
	if(cnt > ndir-off)
		cnt = ndir-off;
	iblock = -1;
	buf = malloc(per*sizeof(SacDir));
	for(i=off; i<off+cnt; i++) {
		j = i/per;
		if(j != iblock) {
			loadblock(buf, blocks + j*8, per*sizeof(SacDir));
			iblock = j;
		}
		j *= per;
		sacstat(buf+i-j, p);
		p += DIRLEN;
	}
	free(buf);
	return cnt*DIRLEN;
}

Sac*
saclookup(Sac *s, char *name)
{
	int ndir;
	int top, bot, i, j, k, per;
	uchar *blocks;
	SacDir *buf;
	int iblock;
	SacDir *sd;
	
	if(strcmp(name, "..") == 0)
		return sacparent(s);
	blocks = data + getv(s->blocks);
	per = blocksize/sizeof(SacDir);
	ndir = getv(s->length);
	buf = malloc(per*sizeof(SacDir));
	iblock = -1;

	if(1) {
		// linear search
		for(i=0; i<ndir; i++) {
			j = i/per;
			if(j != iblock) {
				loadblock(buf, blocks + j*8, per*sizeof(SacDir));
				iblock = j;
			}
			j *= per;
			sd = buf+i-j;
			k = strcmp(name, sd->name);
			if(k == 0) {
//print("walk %s %lld %d\n", name, getv(s->blocks), i);
				s->path = sacpathalloc(s->path, getv(s->blocks), i);
				s->SacDir = *sd;
				free(buf);
				return s;
			}
		}
		free(buf);
		return 0;
	}

	// binary search
	top = ndir;
	bot = 0;
	while(bot != top){
if(bot>top)
sysfatal("binary serach failed: %d %d\n", bot, top);
		i = (bot+top)>>1;
		j = i/per;
		if(j != iblock) {
			loadblock(buf, blocks + j*8, per*sizeof(SacDir));
			iblock = j;
		}
		j *= per;
		sd = buf+i-j;
		k = strcmp(name, sd->name);
		if(k == 0) {
			s->path = sacpathalloc(s->path, getv(s->blocks), i);
			s->SacDir = *sd;
			free(buf);
		}
		if(k < 0) {
			top = i;
			sd = buf;
			if(strcmp(name, sd->name) < 0)
				top = j;
		} else {
			bot = i+1;
			if(ndir-j < per)
				i = ndir-j;
			else
				i = per;
			sd = buf+i-1;
			if(strcmp(name, sd->name) > 0)
				bot = j+i;
		}
	}
	return 0;
}

Fid *
newfid(int fid)
{
	Fid *f, *ff;

	ff = 0;
	for(f = fids; f; f = f->next)
		if(f->fid == fid)
			return f;
		else if(!ff && !f->busy)
			ff = f;
	if(ff){
		ff->fid = fid;
		return ff;
	}
	f = malloc(sizeof *f);
//fprint(2, "newfid\n");
	memset(f, 0 , sizeof(Fid));
	f->fid = fid;
	f->next = fids;
	fids = f;
	return f;
}

void
io(void)
{
	char *err;
	int n;

	for(;;){
		/*
		 * reading from a pipe or a network device
		 * will give an error after a few eof reads
		 * however, we cannot tell the difference
		 * between a zero-length read and an interrupt
		 * on the processes writing to us,
		 * so we wait for the error
		 */
		n = read(mfd[0], mdata, sizeof mdata);
		if(n == 0)
			continue;
		if(n < 0)
			error("mount read");
		if(convM2S(mdata, &rhdr, n) == 0)
			continue;

		if(debug)
			fprint(2, "sacfs:<-%F\n", &rhdr);

		thdr.data = mdata + MAXMSG;
		if(!fcalls[rhdr.type])
			err = "bad fcall type";
		else
			err = (*fcalls[rhdr.type])(newfid(rhdr.fid));
		if(err){
			thdr.type = Rerror;
			strncpy(thdr.ename, err, ERRLEN);
		}else{
			thdr.type = rhdr.type + 1;
			thdr.fid = rhdr.fid;
		}
		thdr.tag = rhdr.tag;
		if(debug)
			fprint(2, "ramfs:->%F\n", &thdr);/**/
		n = convS2M(&thdr, mdata);
		if(write(mfd[1], mdata, n) != n)
			error("mount write");
	}
}

int
perm(Fid *f, Sac *s, int p)
{
	ulong perm = getl(s->mode);
	if((p*Pother) & perm)
		return 1;
	if(strcmp(f->user, s->gid)==0 && ((p*Pgroup) & perm))
		return 1;
	if(strcmp(f->user, s->uid)==0 && ((p*Powner) & perm))
		return 1;
	return 0;
}


ulong
getl(void *p)
{
	uchar *a = p;

	return (a[0]<<24) | (a[1]<<16) | (a[2]<<8) | a[3];
}

vlong
getv(void *p)
{
	uchar *a = p;
	ulong l0, l1;
	vlong v;

	l0 = (a[0]<<24) | (a[1]<<16) | (a[2]<<8) | a[3];
	a += 4;
	l1 = (a[0]<<24) | (a[1]<<16) | (a[2]<<8) | a[3];
	
	v = l0;
	v <<= 32;
	v |= l1;
	return v;
}

Dev sacdevtab = {
	'C',
	"sac",

	devreset,
	sacinit,
	sacattach,
	sacclone,
	sacwalk,
	sacstat,
	sacopen,
	devcreate,
	sacclose,
	sacread,
	devbread,
	sacwrite,
	devbwrite,
	devremove,
	devwstat,
};
