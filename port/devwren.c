#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"io.h"

#include	"scsi.h"

enum {
	Qdir, Qdata, Qstruct,
};

static Dirtab wrendir[]={
	"data",		{Qdata},	0,	0600,
	"struct",	{Qstruct},	8,	0400,
};

#define	NWREN	(sizeof wrendir/sizeof(Dirtab))

static long	maxblock[64];
static long	blocksize[64];

static Scsi	staticcmd;		/* BUG */
static uchar	datablk[4*512];		/* BUG */

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
	return dev;
cant:
	error(Ebadarg);
}

static int
wrengen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dp)
{
	long l;
	if(tab==0 || s>=ntab)
		return -1;
	tab+=s;
	if (tab->qid.path==Qdata && 0<=c->dev && c->dev<64)
		l = maxblock[c->dev]*blocksize[c->dev];
	else
		l = tab->length;
	devdir(c, tab->qid, tab->name, l, tab->perm, dp);
	return 1;
}

void
wrenreset(void)
{}

void
wreninit(void)
{
	Scsi *cmd = &staticcmd;
	cmd->cmd.base = cmd->cmdblk;
	cmd->data.base = datablk;
}

/*
 *  param is #r<target>.<lun>
 */
Chan *
wrenattach(char *param)
{
	uchar buf[32];
	int dev;
	Chan *c;
	dev = wrendev(param);
	scsiready(dev);
	scsisense(dev, buf);
	scsicap(dev, buf);
	c = devattach('r', param);
	c->dev = dev;
	maxblock[dev] = BGLONG(&buf[0]);
	blocksize[dev] = BGLONG(&buf[4]);
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
wrenread(Chan *c, char *a, long n)
{
	Scsi *cmd = &staticcmd;
	unsigned long lbn;
	if (n == 0)
		return 0;
	switch ((int)(c->qid.path & ~CHDIR)) {
	case Qdir:
		return devdirread(c, a, n, wrendir, NWREN, wrengen);
	case Qdata:
		if (n % blocksize[c->dev] || c->offset % blocksize[c->dev])
			error(Ebadarg);
		lbn = c->offset/blocksize[c->dev];
		if (lbn >= maxblock[c->dev])
			error(Ebadarg);
		if (n > sizeof datablk)
			n = sizeof datablk;
		qlock(cmd);
		if (waserror()) {
			qunlock(cmd);
			nexterror();
		}
		cmd->target = c->dev>>3;
		cmd->lun = c->dev&7;
		cmd->cmd.ptr = cmd->cmd.base;
		cmd->cmdblk[0] = 0x08;
		cmd->cmdblk[1] = lbn>>16;
		cmd->cmdblk[2] = lbn>>8;
		cmd->cmdblk[3] = lbn;
		cmd->cmdblk[4] = n/blocksize[c->dev];
		cmd->cmdblk[5] = 0x00;
		cmd->cmd.lim = &cmd->cmdblk[6];
		cmd->data.lim = cmd->data.base + n;
		cmd->data.ptr = cmd->data.base;
		cmd->save = cmd->data.base;
		scsiexec(cmd, 1);
		n = cmd->data.ptr - cmd->data.base;
		memcpy(a, cmd->data.base, n);
		qunlock(cmd);
		break;
	case Qstruct:
		if (n < 8)
			error(Ebadarg);
		if (c->offset >= 8)
			return 0;
		n = 8;
		PLONG((uchar *)&a[0], maxblock[c->dev]);
		PLONG((uchar *)&a[4], blocksize[c->dev]);
		break;
	default:
		panic("wrenread");
	}
	return n;
}

long
wrenwrite(Chan *c, char *a, long n)
{
	Scsi *cmd = &staticcmd;
	unsigned long lbn;
	if (n == 0)
		return 0;
	switch ((int)(c->qid.path & ~CHDIR)) {
	case Qdata:
		if (n % blocksize[c->dev] || c->offset % blocksize[c->dev])
			error(Ebadarg);
		lbn = c->offset/blocksize[c->dev];
		if (lbn >= maxblock[c->dev])
			error(Ebadarg);
		if (n > sizeof datablk)
			n = sizeof datablk;
		qlock(cmd);
		if (waserror()) {
			qunlock(cmd);
			nexterror();
		}
		cmd->target = c->dev>>3;
		cmd->lun = c->dev&7;
		cmd->cmd.ptr = cmd->cmd.base;
		cmd->cmdblk[0] = 0x0a;
		cmd->cmdblk[1] = lbn>>16;
		cmd->cmdblk[2] = lbn>>8;
		cmd->cmdblk[3] = lbn;
		cmd->cmdblk[4] = n/blocksize[c->dev];
		cmd->cmdblk[5] = 0x00;
		cmd->cmd.lim = &cmd->cmdblk[6];
		cmd->data.lim = cmd->data.base + n;
		cmd->data.ptr = cmd->data.base;
		cmd->save = cmd->data.base;
		memcpy(cmd->data.base, a, n);
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
