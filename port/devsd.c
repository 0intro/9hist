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

enum {
	TypeDA		= 0x00,		/* Direct Access */
	TypeSA		= 0x01,		/* Sequential Access */
	TypeWO		= 0x04,		/* Worm */
	TypeCD		= 0x05,		/* CD-ROM */
	TypeMO		= 0x07,		/* rewriteable Magneto-Optical */
	TYpeMC		= 0x08,		/* Medium Changer */
};

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

static int types[] = {
	TypeDA, TypeCD, TypeMO,
	-1,
};

static int
sdgen(Chan *c, Dirtab*, long, long s, Dir *dirp)
{
	Qid qid;
	Disk *d;
	Part *p;
	int unit;
	char name[2*NAMELEN];

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
	uchar scratch[0xFF], type;
	int dev, i, nbytes;

	dev = 0;
	for(;;) {
		d = &disk[ndisk];
		dev = scsiinv(dev, types, &d->t, &d->inquire, d->id);
		if(dev < 0)
			break;

		/*
		 * Search for all the lun's.
		 */
		for(i = 0; i < Nlun; i++) {
			d->lun = i;

			/*
			 * A SCSI target does not support a lun if the
			 * the peripheral device type and qualifier fields
			 * in the response to an inquiry command are 0x7F.
			 */
			nbytes = sizeof(scratch);
			memset(scratch, 0, nbytes);
			if(scsiinquiry(d->t, d->lun, scratch, &nbytes) != STok)
				continue;
			if(scratch[0] == 0x7F)
				continue;
			type = scratch[0] & 0x1F;

			/*
			 * Read-capacity is mandatory for TypeDA, TypeMO and TypeCD.
			 * It may return 'not ready' if TypeDA is not spun up,
			 * TypeMO or TypeCD are not loaded or just plain slow getting
			 * their act together after a reset.
			 * If 'not ready' comes back, try starting a TypeDA and punt
			 * the get capacity until the drive is attached.
			 * It might be possible to be smarter here, and look at the
			 * response from a test-unit-ready which would show if the
			 * target was in the process of becoming ready.
			 */
			if(scsicap(d->t, d->lun, &d->size, &d->bsize) != STok) {
				nbytes = sizeof(scratch);
				memset(scratch, 0, nbytes);
				scsireqsense(d->t, 0, scratch, &nbytes, 1);
				if((scratch[2] & 0x0F) != 0x02)
					continue;
				if(type == TypeDA)
					scsistart(d->t, d->lun, 0);
				d->size = d->bsize = 0;
			}

			switch(type){

			case TypeDA:
			case TypeMO:
				sprint(d->vol, "sd%d", ndisk);
				break;

			case TypeCD:
				sprint(d->vol, "cd%d", ndisk);
				break;

			default:
				continue;
			}

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
sdcreate(Chan*, char*, int, ulong)
{
	error(Eperm);
}

void
sdremove(Chan*)
{
	error(Eperm);
}

void
sdwstat(Chan*, char*)
{
	error(Eperm);
}

void
sdclose(Chan *c)
{
	Disk *d;
	Part *p;

	if(c->qid.path & CHDIR)
		return;

	d = &disk[DRIVE(c->qid)];
	p = &d->table[PART(c->qid)];
	if((c->mode&3) != OREAD && strcmp(p->name, "partition") == 0)
		sdrdpart(d);
}

long
sdread(Chan *c, void *a, long n, ulong offset)
{
	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, sdgen);

	return sdio(c, 0, a, n, offset);
}

Block*
sdbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
sdwrite(Chan *c, char *a, long n, ulong offset)
{
	return sdio(c, 1, a, n, offset);
}

long
sdbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

static void
sdrdpart(Disk *d)
{
	Part *p;
	int n, i;
	char *b, *line[Npart+2], *field[3];
	static char MAGIC[] = "plan9 partitions";

	/*
	 * If the drive wasn't ready when we tried to do a
	 * read-capacity earlier (in sdinit()), try again.
	 * It might be possible to be smarter here, and look at the
	 * response from a test-unit-ready which would show if the
	 * target was in the process of becoming ready.
	 */
	if(d->size == 0 || d->bsize == 0){
		if(scsicap(d->t, d->lun, &d->size, &d->bsize) != STok){
			d->size = d->bsize = 0;
			error(Eio);
		}
	}

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

	if((d->inquire[0] & 0x1F) == TypeCD){
		scsifree(b);
		qunlock(d);
		return;
	}

	scsibio(d->t, d->lun, SCSIread, b, 1, d->bsize, d->table[0].end-1);
	b[d->bsize-1] = '\0';

	/*
	 *  parse partition table.
	 */
	n = getfields(b, line, Npart+2, "\n");
	if(n > 0 && strncmp(line[0], MAGIC, sizeof(MAGIC)-1) == 0) {
		for(i = 1; i < n; i++) {
			switch(getfields(line[i], field, 3, " ")) {
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

	if(write && (d->inquire[0] & 0x1F) == TypeCD)
		error(Eperm);

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
