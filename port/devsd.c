/*
 *  SCSI disc driver
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"

enum
{
	LogNpart	= 4,
	Npart		= 1<<LogNpart,
	Ndisk		= 32,
	Nlun		= 8,
};
#define DRIVE(qid)	((qid).path>>LogNpart)
#define PART(qid)	((qid).path&(Npart-1))

typedef struct Part Part;
typedef struct Disk Disk;

struct Part
{
	ulong	beg;
	ulong	end;
	char	name[NAMELEN];
};

struct Disk
{
	QLock;
	Target*	t;
	uchar	lun;
	char	id[NAMELEN];
	char	vol[NAMELEN];

	uchar*	inquire;
	ulong	size;
	ulong	bsize;

	int	npart;
	Part	table[Npart];
};

int	ndisk;
Disk	disk[Ndisk];

static	void	sdrdpart(Disk*);
static	long	sdio(Chan*, int, char*, ulong, ulong);

static int
sdgen(Chan *c, Dirtab *tab, long ntab, long s, Dir *dirp)
{
	Qid qid;
	Disk *d;
	Part *p;
	int unit;
	char name[2*NAMELEN];

	USED(tab, ntab);

	d = disk;
	while(s >= d->npart) {
		s -= d->npart;
		d++;
	}
	unit = d - disk;
	if(unit > ndisk)
		return -1;

	p = d->table+s;
	sprint(name, "%s%s", d->vol, p->name);
	name[NAMELEN-1] = '\0';
	qid = (Qid){(unit<<LogNpart)+s, 0};
	devdir(c, qid, name, (p->end - p->beg) * d->bsize, eve, 0666, dirp);
	return 1;
}

void
sdreset(void)
{
}

void
sdinit(void)
{
	Disk *d;
	ulong s, b;
	int dev, i;

	dev = 0;
	for(;;) {
		d = &disk[ndisk];
		dev = scsiinv(dev, 0, &d->t, &d->inquire, d->id);
		if(dev < 0)
			break;

		if(scsistart(d->t, 0, 1) != STok)
			continue;

		/* Search for other lun's */
		for(i = 0; i < Nlun; i++) {
			d->lun = i;
			scsireqsense(d->t, d->lun, 1);

			/* NCR Raid only seems to answer second capacity
			 * command if lun != 0
			 */
			if(scsicap(d->t, d->lun, &s, &b) != STok) {
				scsireqsense(d->t, 0, 1);
				continue;
			}
			scsireqsense(d->t, 0, 1);

			s = 0;
			b = 0;
			if(scsicap(d->t, d->lun, &s, &b) != STok) {
				scsireqsense(d->t, 0, 1);
				continue;
			}

			if(scsireqsense(d->t, d->lun, 1) != STok)
				continue;

			if(s == 0 || b == 0)
				continue;

			d->size = s;
			d->bsize = b;
			sprint(d->vol, "sd%d", ndisk);

			if(++ndisk >= Ndisk)
				break;
			d++;
			d->t = d[-1].t;
			d->inquire = d[-1].inquire;
			strcpy(d->id, d[-1].id);
		}

		if(ndisk >= Ndisk) {
			print("devsd: configure more disks\n");
			break;
		}
	}
	print("sd out\n");
}

Chan*
sdattach(char *spec)
{
	int i;

	for(i = 0; i < ndisk; i++)
		sdrdpart(&disk[i]);
	
	return devattach('w', spec);
}

Chan *
sdclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
sdwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, sdgen);
}

void
sdstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, sdgen);
}

Chan *
sdopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, sdgen);
}

void
sdcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
sdremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
sdwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
sdclose(Chan *c)
{
	Disk *d;
	Part *p;

	if(c->mode != OWRITE && c->mode != ORDWR)
		return;

	d = &disk[DRIVE(c->qid)];
	p = &d->table[PART(c->qid)];
	if(strcmp(p->name, "partition"))
		return;

	sdrdpart(d);
}

long
sdread(Chan *c, void *a, long n, ulong offset)
{
	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, 0, 0, sdgen);

	return sdio(c, 0, a, n, offset);
}

long
sdwrite(Chan *c, char *a, long n, ulong offset)
{
	return sdio(c, 1, a, n, offset);
}

static void
sdrdpart(Disk *d)
{
	Part *p;
	int n, i;
	char *b, *line[Npart+2], *field[3];
	static char MAGIC[] = "plan9 partitions";

	b = scsialloc(d->bsize);
	if(b == 0)
		error(Enomem);

	qlock(d);
	
	p = d->table;

	strcpy(p->name, "disk");
	p->beg = 0;
	p->end = d->size + 1;
	p++;
	strcpy(p->name, "partition");
	p->beg = d->table[0].end - 1;
	p->end = d->table[0].end;
	p++;
	d->npart = 2;

	scsibio(d->t, d->lun, SCSIread, b, 1, d->bsize, d->table[0].end-1);
	b[d->bsize-1] = '\0';

	/*
	 *  parse partition table.
	 */
	n = getfields(b, line, Npart+2, '\n');
	if(n > 0 && strncmp(line[0], MAGIC, sizeof(MAGIC)-1) == 0) {
		for(i = 1; i < n; i++) {
			switch(getfields(line[i], field, 3, ' ')) {
			case 2:
				if(strcmp(field[0], "unit") == 0)
					strncpy(d->vol, field[1], NAMELEN);
				break;	
			case 3:
				if(p >= &d->table[Npart])
					break;
				strncpy(p->name, field[0], NAMELEN);
				p->beg = strtoul(field[1], 0, 0);
				p->end = strtoul(field[2], 0, 0);
				if(p->beg > p->end || p->beg >= d->table[0].end)
					break;
				p++;
			}
		}
	}
	d->npart = p - d->table;
	scsifree(b);
	qunlock(d);
}

static long
sdio(Chan *c, int write, char *a, ulong len, ulong offset)
{
	Disk *d;
	Part *p;
	uchar *b;
	ulong block, n, max, x;

	d = &disk[DRIVE(c->qid)];
	p = &d->table[PART(c->qid)];

	block = (offset / d->bsize) + p->beg;
	n = (offset + len + d->bsize - 1) / d->bsize + p->beg - block;
	max = SCSImaxxfer / d->bsize;
	if(n > max)
		n = max;
	if(block + n > p->end)
		n = p->end - block;
	if(block >= p->end || n == 0)
		return 0;

	b = scsialloc(n*d->bsize);
	if(b == 0)
		error(Enomem);
	if(waserror()) {
		scsifree(b);
		nexterror();
	}
	offset %= d->bsize;
	if(write) {
		if(offset || len % d->bsize) {
			x = scsibio(d->t, d->lun, SCSIread, b, n, d->bsize, block);
			if(x < n * d->bsize) {
				n = x / d->bsize;
				x = n * d->bsize - offset;
				if(len > x)
					len = x;
			}
		}
		memmove(b + offset, a, len);
		x = scsibio(d->t, d->lun, SCSIwrite, b, n, d->bsize, block);
		if(x < offset)
			len = 0;
		else
		if(len > x - offset)
			len = x - offset;
	}
	else {
		x = scsibio(d->t, d->lun, SCSIread, b, n, d->bsize, block);
		if(x < offset)
			len = 0;
		else
		if(len > x - offset)
			len = x - offset;
		memmove(a, b+offset, len);
	}
	poperror();
	scsifree(b);
	return len;
}
