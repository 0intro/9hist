#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"io.h"

typedef struct Partition	Partition;
typedef struct Drive		Drive;

enum {
	Npart=		8+2,	/* 8 sub partitions, disk, and partition */
	Ndisk=		64,	/* maximum disks */

	/* file types */
	Qdir=		0,
};
#define PART(x)		((x)&0xF)
#define DRIVE(x)	(((x)>>4)&0x7)
#define MKQID(d,p)	(((d)<<4) | (p))

struct Partition
{
	ulong	start;
	ulong	end;
	char	name[NAMELEN+1];
};

struct Drive
{
	ulong		bytes;			/* bytes per block */
	int		npart;			/* actual number of partitions */
	int		drive;
	Partition	p[Npart];
};

static Drive	wren[Ndisk];

#define	DATASIZE	(8*1024)	/* BUG */

static void	wrenpart(int);
static long	wrenio(Drive *, Partition *, int, char *, ulong, ulong);

/*
 *  accepts [0-7].[0-7], or abbreviation
 */
static int
wrendev(char *p)
{
	int dev = 0;

	if(p == 0 || p[0] == 0)
		goto out;
	if(p[0] < '0' || p[0] > '7')
		goto cant;
	dev = (p[0] - '0') << 3;
	if(p[1] == 0)
		goto out;
	if(p[1] != '.')
		goto cant;
	if(p[2] == 0)
		goto out;
	if(p[2] < '0' || p[2] > '7')
		goto cant;
	dev |= p[2] - '0';
	if(p[3] != 0)
		goto cant;
out:
	if(dev >= Ndisk)
		error(Ebadarg);
	return dev;
cant:
	error(Ebadarg);
}

static int
wrengen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dirp)
{
	Qid qid;
	int drive;
	char name[NAMELEN+4];
	Drive *dp;
	Partition *pp;
	ulong l;

	qid.vers = 0;
	drive = s/Npart;
	s = s % Npart;
	if(drive >= Ndisk)
		return -1;
	dp = &wren[drive];

	if(s >= dp->npart)
		return 0;

	pp = &dp->p[s];
	sprint(name, "hd%d%s", drive, pp->name);
	name[NAMELEN] = 0;
	qid.path = MKQID(drive, s);
	l = (pp->end - pp->start) * dp->bytes;
	devdir(c, qid, name, l, 0600, dirp);
	return 1;
}

void
wrenreset(void)
{
}

void
wreninit(void)
{
}

/*
 *  param is #r<target>.<lun>
 */
Chan *
wrenattach(char *param)
{
	Chan *c;
	int drive;

	drive = wrendev(param);
	wrenpart(drive);
	c = devattach('r', param);
	c->dev = drive;
	return c;
}

Chan*
wrenclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
wrenwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, wrengen);
}

void
wrenstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, wrengen);
}

Chan*
wrenopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, wrengen);
}

void
wrencreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
wrenclose(Chan *c)
{
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

long
wrenread(Chan *c, char *a, long n, ulong offset)
{
	Drive *d;
	Partition *p;


	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, 0, 0, wrengen);

	d = &wren[DRIVE(c->qid.path)];
	p = &d->p[PART(c->qid.path)];
	return wrenio(d, p, 0, a, n, offset);
}

long
wrenwrite(Chan *c, char *a, long n, ulong offset)
{
	Drive *d;
	Partition *p;

	d = &wren[DRIVE(c->qid.path)];
	p = &d->p[PART(c->qid.path)];
	return wrenio(d, p, 1, a, n, offset);
}

static long
wrenio(Drive *d, Partition *p, int write, char *a, ulong n, ulong offset)
{
	Scsi *cmd;
	void *b;
	ulong block;

	if(n % d->bytes || offset % d->bytes)
		error(Ebadarg);
	block = offset / d->bytes + p->start;
	if(block >= p->end)
		return 0;
	if(n > DATASIZE)
		n = DATASIZE;
	n /= d->bytes;
	if(block + n > p->end)
		n = p->end - block;
	if(n == 0)
		return 0;
	if(write)
		cmd = scsicmd(d->drive, 0x0a, n*d->bytes);
	else
		cmd = scsicmd(d->drive, 0x08, n*d->bytes);
	if(waserror()){
		qunlock(cmd);
		nexterror();
	}
	cmd->cmdblk[1] = block>>16;
	cmd->cmdblk[2] = block>>8;
	cmd->cmdblk[3] = block;
	cmd->cmdblk[4] = n;
	if(write)
		memmove(cmd->data.base, a, n*d->bytes);
	scsiexec(cmd, !write);
	n = cmd->data.ptr - cmd->data.base;
	if(!write)
		memmove(a, cmd->data.base, n);
	qunlock(cmd);
	poperror();
	return n;
}

/*
 *  read partition table.  The partition table is just ascii strings.
 */
#define MAGIC "plan9 partitions"
static void
wrenpart(int dev)
{
	Scsi *cmd;
	Drive *dp;
	Partition *pp;
	uchar buf[32];
	char *b;
	char *line[Npart+1];
	char *field[3];
	ulong n;
	int i;

	scsiready(dev);
	scsisense(dev, buf);
	scsicap(dev, buf);
	dp = &wren[dev];
	dp->drive = dev;
	if(dp->npart)
		return;
	/*
	 *  we always have a partition for the whole disk
	 *  and one for the partition table
	 */
	dp->bytes = (buf[4]<<24)+(buf[5]<<16)+(buf[6]<<8)+(buf[7]);
	pp = &dp->p[0];
	strcpy(pp->name, "disk");
	pp->start = 0;
	pp->end = (buf[0]<<24)+(buf[1]<<16)+(buf[2]<<8)+(buf[3]) + 1;
	pp++;
	strcpy(pp->name, "partition");
	pp->start = dp->p[0].end - 1;
	pp->end = dp->p[0].end;
	dp->npart = 2;

	/*
	 *  read partition table from disk, null terminate
	 */
	cmd = scsicmd(dev, 0x08, dp->bytes);
	if(waserror()){
		qunlock(cmd);
		nexterror();
	}
	n = dp->p[0].end-1;
	cmd->cmdblk[1] = n>>16;
	cmd->cmdblk[2] = n>>8;
	cmd->cmdblk[3] = n;
	cmd->cmdblk[4] = 1;
	scsiexec(cmd, 1);
	cmd->data.base[dp->bytes-1] = 0;

	/*
	 *  parse partition table.
	 */
	n = getfields((char *)cmd->data.base, line, Npart+1, '\n');
	if(strncmp(line[0], MAGIC, sizeof(MAGIC)-1) != 0)
		goto out;
	for(i = 1; i < n; i++){
		pp++;
		if(getfields(line[i], field, 3, ' ') != 3){
			break;
		}
		strncpy(pp->name, field[0], NAMELEN);
		pp->start = strtoul(field[1], 0, 0);
		pp->end = strtoul(field[2], 0, 0);
		if(pp->start > pp->end || pp->start >= dp->p[0].end){
			break;
		}
		dp->npart++;
	}
out:
	qunlock(cmd);
	poperror();
}
