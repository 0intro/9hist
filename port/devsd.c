/*
 *  SCSI disc driver
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum
{
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
	int	lun;
	char	id[NAMELEN];
	char	vol[NAMELEN];

	uchar*	inquire;
	int	partok;
	ulong	version;
	ulong	size;
	ulong	bsize;

	int	npart;
	Part	table[Npart];
};

int	ndisk;
Disk	disk[Ndisk];

static	int	sdrdpart(Disk*);
static	long	sdio(Chan*, int, char*, ulong, vlong);

static int types[] =
{
	TypeDA, TypeCD, TypeMO,
	-1,
};

static int
sdgen(Chan *c, Dirtab*, int, int s, Dir *dirp)
{
	Qid qid;
	Disk *d;
	Part *p;
	int unit;
	char name[2*NAMELEN];
	vlong l;

	if(s == DEVDOTDOT){
		devdir(c, qid, "#w", 0, eve, 0555, dirp);
		return 1;
	}

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
	l = (p->end - p->beg) * (vlong)d->bsize;
	devdir(c, qid, name, l, eve, 0666, dirp);
	return 1;
}

static void
sdinit(void)
{
	Disk *d;
	uchar *scratch, type;
	int dev, i, nbytes;

	dev = 0;
	scratch = scsialloc(0xFF);
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
			nbytes = 0xFF;
			memset(scratch, 0, nbytes);

			if(scsiinquiry(d->t, d->lun, scratch, &nbytes) != STok)
				continue;
			if(scratch[0] == 0x7F)
				continue;
			type = scratch[0] & 0x1F;

			/*
			 * Read-capacity is mandatory for TypeDA, TypeMO and
			 * TypeCD. It may return 'not ready' if TypeDA is not
			 * spun up, TypeMO or TypeCD are not loaded or just
			 * plain slow getting their act together after a reset.
			 * If 'not ready' comes back, try starting a TypeDA and
			 * punt the get capacity until the drive is attached.
			 */
			if(scsicap(d->t, d->lun, &d->size, &d->bsize) != STok) {
				nbytes = 0xFF;
				memset(scratch, 0, nbytes);
				scsireqsense(d->t, 0, scratch, &nbytes, 1);
				switch(scratch[2] & 0x0F){

				case 0x00:
				case 0x01:
				case 0x02:
					break;
				case 0x06:
					if(scratch[12] == 0x28 && !scratch[13])
						break;
					if(scratch[12] == 0x29 && !scratch[13])
						break;
					/*FALLTHROUGH*/
				default:
					continue;
				}
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
	scsifree(scratch);
}

static Chan*
sdattach(char *spec)
{
	int i;

	for(i = 0; i < ndisk; i++){
		qlock(&disk[i]);
		if(disk[i].partok == 0)
			sdrdpart(&disk[i]);
		qunlock(&disk[i]);
	}

	return devattach('w', spec);
}

static int
sdwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, sdgen);
}

static void
sdstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, sdgen);
}

static Chan*
sdopen(Chan *c, int omode)
{
	return devopen(c, omode, 0, 0, sdgen);
}

static void
sdclose(Chan *c)
{
	Disk *d;
	Part *p;

	if(c->qid.path & CHDIR)
		return;

	d = &disk[DRIVE(c->qid)];
	p = &d->table[PART(c->qid)];
	if((c->mode&3) != OREAD && strcmp(p->name, "partition") == 0){
		qlock(d);
		d->partok = 0;
		sdrdpart(d);
		qunlock(d);
	}
}

static long
sdread(Chan *c, void *a, long n, vlong off)
{

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, sdgen);

	return sdio(c, 0, a, n, off);
}

static long
sdwrite(Chan *c, void *a, long n, vlong off)
{
	Disk *d;

	d = &disk[DRIVE(c->qid)];

	if((d->inquire[0] & 0x1F) == TypeCD)
		error(Eperm);
	return sdio(c, 1, a, n, off);
}

Dev sddevtab = {
	'w',
	"sd",

	devreset,
	sdinit,
	sdattach,
	devclone,
	sdwalk,
	sdstat,
	sdopen,
	devcreate,
	sdclose,
	sdread,
	devbread,
	sdwrite,
	devbwrite,
	devremove,
	devwstat,
};

static int
sdrdpart(Disk *d)
{
	Part *p;
	int n, i;
	char *b, *line[Npart+2], *field[3];
	static char MAGIC[] = "plan9 partitions";

	if(d->partok)
		return STok;

	p = d->table;
	strcpy(p->name, "disk");
	p->beg = 0;
	p->end = 0;
	d->npart = 1;

	/*
	 * If the drive wasn't ready when a read-capacity was tried
	 * earlier (in sdinit()), try again.
	 */
	if(d->size == 0 || d->bsize == 0){
		n = scsicap(d->t, d->lun, &d->size, &d->bsize);
		if(n != STok){
			d->size = d->bsize = 0;
			return scsierrstr(n);
		}
	}

	p->end = d->size + 1;

	if((d->inquire[0] & 0x1F) == TypeCD)
		return STok;

	b = scsialloc(d->bsize);
	if(b == 0)
		return scsierrstr(STnomem);

	p++;
	strcpy(p->name, "partition");
	p->beg = d->table[0].end - 2;
	p->end = d->table[0].end - 1;
	p++;
	d->npart = 2;

	/*
	 *  Read second last sector from disk, null terminate.
	 *  The last sector used to hold the partition tables.
	 *  However, this sector is special on some PC's so we've
	 *  started to use the second last sector as the partition
	 *  table instead.  To avoid reconfiguring all our old systems
	 *  we still check if there is a valid partition table in
	 *  the last sector if none is found in the second last.
	 */
	scsibio(d->t, d->lun, SCSIread, b, 1, d->bsize, d->table[0].end-2);
	b[d->bsize-1] = '\0';
	n = parsefields(b, line, nelem(line), "\n");
	if(n <= 0 || strncmp(line[0], MAGIC, sizeof(MAGIC)-1) != 0){
		/* try the last */
		scsibio(d->t, d->lun, SCSIread, b, 1, d->bsize, d->table[0].end-1);
		b[d->bsize-1] = '\0';
		n = parsefields(b, line, nelem(line), "\n");
		/* only point partition file at last sector if there is one there */
		if(n > 0 && strncmp(line[0], MAGIC, sizeof(MAGIC)-1) == 0){
			d->table[1].beg++;
			d->table[1].end++;
		}
	}

	/*
	 *  parse partition table.
	 */
	if(n > 0 && strncmp(line[0], MAGIC, sizeof(MAGIC)-1) == 0) {
		for(i = 1; i < n; i++) {
			if(parsefields(line[i], field, nelem(field), " ") != 3)
				break;
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
	d->npart = p - d->table;
	scsifree(b);

	d->partok = 1;

	return STok;
}

static long
sdio(Chan *c, int write, char *a, ulong len, vlong off)
{
	Disk *d;
	Part *p;
	uchar *b;
	ulong block, n, max, x;
	ulong offset;

	d = &disk[DRIVE(c->qid)];
	p = &d->table[PART(c->qid)];

	block = (off / d->bsize) + p->beg;
	n = (off + len + d->bsize - 1) / d->bsize + p->beg - block;
	max = SCSImaxxfer / d->bsize;
	if(n > max)
		n = max;
	if(block + n > p->end)
		n = p->end - block;
	if(block >= p->end || n == 0)
		return 0;

	b = scsialloc(n*d->bsize);
	if(b == 0)
		return scsierrstr(STnomem);

	offset = off % d->bsize;
	if(write) {
		if(offset || len % d->bsize) {
			x = scsibio(d->t, d->lun, SCSIread, b, n, d->bsize, block);
			if(x < 0){
				len = -1;
				goto buggery;
			}
			if(x < n * d->bsize) {
				n = x / d->bsize;
				x = n * d->bsize - offset;
				if(len > x)
					len = x;
			}
		}
		memmove(b + offset, a, len);
		x = scsibio(d->t, d->lun, SCSIwrite, b, n, d->bsize, block);
		if(x < 0){
			len = -1;
			goto buggery;
		}
		if(x < offset)
			len = 0;
		else
		if(len > x - offset)
			len = x - offset;
	}
	else {
		x = scsibio(d->t, d->lun, SCSIread, b, n, d->bsize, block);
		if(x < 0){
			len = -1;
			goto buggery;
		}
		if(x < offset)
			len = 0;
		else
		if(len > x - offset)
			len = x - offset;
		memmove(a, b+offset, len);
	}

buggery:
	scsifree(b);
	return len;
}
