#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"io.h"

typedef struct Part	Part;
typedef struct Disk	Disk;

enum {
	Npart=		2,	/* maximum partitions per disk */
	Ndisk=		64,	/* maximum disks */

	Qdir=		0,
	Qdata=		16,
	Qstruct=	32,

	Mask=		0x7,
};

static Dirtab *wrendir;
#define	NWREN	(2*(Npart+1))

struct Part
{
	ulong 	firstblock;
	ulong 	maxblock;
};
struct Disk
{
	ulong	blocksize;
	Part	p[Npart];
};

static Disk	wren[Ndisk];

#define	DATASIZE	(8*1024)	/* BUG */

#define	BGLONG(p)	(((((((p)[0]<<8)|(p)[1])<<8)|(p)[2])<<8)|(p)[3])

/*
 *  accepts [0-7].[0-7], or abbreviation
 */
static int
wrendev(char *p)
{
	int dev = 0;
	if (p==0 || p[0]==0)
		goto out;
	if (p[0]<'0' || p[0]>'7')
		goto cant;
	dev = (p[0]-'0')<<3;
	if (p[1]==0)
		goto out;
	if (p[1]!='.')
		goto cant;
	if (p[2]==0)
		goto out;
	if (p[2]<'0' || p[2]>'7')
		goto cant;
	dev |= p[2]-'0';
	if (p[3]!=0)
		goto cant;
out:
	if(dev >= Ndisk)
		error(Ebadarg);
	return dev;
cant:
	error(Ebadarg);
}

static int
wrengen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dp)
{
	long l;
	Part *p;
	Disk *d;

	if(s >= ntab)
		return -1;
	if(c->dev >= Ndisk)
		return -1;

	tab += s;
	d = &wren[c->dev];
	p = &d->p[tab->qid.path&Mask];
	if((tab->qid.path&~Mask) == Qdata)
		l = d->blocksize * (p->maxblock - p->firstblock);
	else
		l = 8;
	devdir(c, tab->qid, tab->name, l, tab->perm, dp);
	return 1;
}

void
wrenreset(void)
{
	Dirtab *p;
	int i;

	p = wrendir = ialloc((Npart+1) * 2 * sizeof(Dirtab), 0);
	for(i = 0; i < Npart; i++){
		sprint(p->name, "data%d", i);
		p->qid.path = Qdata + i;
		p->perm = 0600;
		p++->length = 0;
		sprint(p->name, "struct%d", i);
		p->qid.path = Qstruct + i;
		p->perm = 0600;
		p++->length = 0;
	}
	strcpy(p->name, "data");
	p->qid.path = Qdata + Npart;
	p->perm = 0600;
	p++->length = 0;
	strcpy(p->name, "struct");
	p->qid.path = Qstruct + Npart;
	p->perm = 0600;
	p->length = 0;
}

void
wreninit(void)
{}

/*
 *  param is #r<target>.<lun>
 */
Chan *
wrenattach(char *param)
{
	uchar buf[32];
	int dev;
	Chan *c;
	Disk *d;
	ulong plen;
	int i;

	dev = wrendev(param);
	scsiready(dev);
	scsisense(dev, buf);
	scsicap(dev, buf);
	c = devattach('r', param);
	c->dev = dev;
	d = &wren[dev];
	d->blocksize = BGLONG(&buf[4]);
	plen = BGLONG(&buf[0]);
	d->p[Npart].firstblock = 0;
	d->p[Npart].maxblock = plen;
	plen = plen/Npart;
	for(i = 0; i < Npart; i++){
		d->p[i].firstblock = i*plen;
		d->p[i].maxblock = (i+1)*plen;
	}
	return c;
}

Chan *
wrenclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
wrenwalk(Chan *c, char *name)
{
	return devwalk(c, name, wrendir, NWREN, wrengen);
}

void
wrenstat(Chan *c, char *db)
{
	devstat(c, db, wrendir, NWREN, wrengen);
}

Chan *
wrenopen(Chan *c, int omode)
{
	if (c->qid.path == Qdata && scsiready(c->dev) != 0)
		error(Eio);
	return devopen(c, omode, wrendir, NWREN, wrengen);
}

void
wrencreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
wrenclose(Chan *c)
{}

#define	PSHORT(p, v)		((p)[0]=(v), (p)[1]=((v)>>8))
#define	PLONG(p, v)		(PSHORT(p, (v)), PSHORT(p+2, (v)>>16))
long
wrenread(Chan *c, char *a, long n, ulong offset)
{
	Scsi *cmd;
	ulong lbn;
	Part *p;
	Disk *d;

	if (n == 0)
		return 0;

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, wrendir, NWREN, wrengen);

	d = &wren[c->dev];
	p = &(d->p[Mask&c->qid.path]);
	switch ((int)(c->qid.path & ~Mask)) {
	case Qdata:
		if (n % d->blocksize || offset % d->blocksize)
			error(Ebadarg);
		lbn = (offset/d->blocksize) + p->firstblock;
		if (lbn >= p->maxblock)
			error(Ebadarg);
		if (n > DATASIZE)
			n = DATASIZE;
		cmd = scsicmd(c->dev, 0x08, n);
		if (waserror()) {
			qunlock(cmd);
			nexterror();
		}
		cmd->cmdblk[1] = lbn>>16;
		cmd->cmdblk[2] = lbn>>8;
		cmd->cmdblk[3] = lbn;
		cmd->cmdblk[4] = n/d->blocksize;
		scsiexec(cmd, 1);
		n = cmd->data.ptr - cmd->data.base;
		memmove(a, cmd->data.base, n);
		qunlock(cmd);
		break;
	case Qstruct:
		if (n < 8)
			error(Ebadarg);
		if (offset >= 8)
			return 0;
		n = 8;
		PLONG((uchar *)&a[0], p->maxblock - p->firstblock);
		PLONG((uchar *)&a[4], d->blocksize);
		break;
	default:
		panic("wrenread");
	}
	return n;
}

long
wrenwrite(Chan *c, char *a, long n, ulong offset)
{
	Scsi *cmd;
	ulong lbn;
	Part *p;
	Disk *d;

	if (n == 0)
		return 0;

	d = &wren[c->dev];
	p = &(d->p[Mask&c->qid.path]);
	switch ((int)(c->qid.path & ~Mask)) {
	case Qdata:
		if (n % d->blocksize || offset % d->blocksize)
			error(Ebadarg);
		lbn = offset/d->blocksize + p->firstblock;
		if (lbn >= p->maxblock)
			error(Ebadarg);
		if (n > DATASIZE)
			n = DATASIZE;
		cmd = scsicmd(c->dev, 0x0a, n);
		if (waserror()) {
			qunlock(cmd);
			nexterror();
		}
		cmd->cmdblk[1] = lbn>>16;
		cmd->cmdblk[2] = lbn>>8;
		cmd->cmdblk[3] = lbn;
		cmd->cmdblk[4] = n/d->blocksize;
		memmove(cmd->data.base, a, n);
		scsiexec(cmd, 0);
		n = cmd->data.ptr - cmd->data.base;
		qunlock(cmd);
		break;
	default:
		panic("wrenwrite");
	}
	return n;
}

void
wrenremove(Chan *c)
{
	error(Eperm);
}

void
wrenwstat(Chan *c, char *dp)
{
	error(Eperm);
}
