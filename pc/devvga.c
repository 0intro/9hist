#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"

/*
 *  Driver for various VGA cards
 */

char	monitor[NAMELEN];	/* monitor name and type */
char	vgacard[NAMELEN];	/* vga card type */
struct screeninfo {
	int	maxx, maxy;	/* current bits per screen */
	int	packed;		/* 0=planar, 1=packed */
	int	interlaced;	/* != 0 if interlaced */
} screeninfo;

enum {
	Qdir=		0,
	Qvgamonitor=	1,
	Qvgasize=	2,
	Qvgatype=	3,
	Qvgaportio=	4,
	Nvga=		4,
};

Dirtab vgadir[]={
	"vgamonitor",	{Qvgamonitor},	0,		0666,
	"vgatype",	{Qvgatype},	0,		0666,
	"vgasize",	{Qvgasize},	0,		0666,
	"vgaportio",	{Qvgaportio},	0,		0666,
};

/* a routine from ../port/devcons.c */
extern	int readstr(ulong, char *, ulong, char *);

void
vgasetup(void) {
}

void
vgareset(void) {
	strcpy(monitor, "generic");
	strcpy(vgacard, "generic");
	screeninfo.maxx = 640;
	screeninfo.maxy = 480;
	screeninfo.packed = 0;
	screeninfo.interlaced = 0;
}

void
vgainit(void)
{
}

Chan*
vgaattach(char *upec)
{
	return devattach('v', upec);
}

Chan*
vgaclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
vgawalk(Chan *c, char *name)
{
	return devwalk(c, name, vgadir, Nvga, devgen);
}

void
vgastat(Chan *c, char *dp)
{
	switch(c->qid.path){
	default:
		devstat(c, dp, vgadir, Nvga, devgen);
		break;
	}
}

Chan*
vgaopen(Chan *c, int omode)
{
	switch(c->qid.path){
	case Qvgamonitor:
	case Qvgatype:
	case Qvgasize:
	case Qvgaportio:
		break;
	}

	return devopen(c, omode, vgadir, Nvga, devgen);
}

void
vgacreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
vgaclose(Chan *c)
{
}

long
vgaread(Chan *c, void *buf, long n, ulong offset)
{
	char obuf[60];
	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, vgadir, Nvga, devgen);
	case Qvgamonitor:
		return readstr(offset, buf, n, monitor);
	case Qvgatype:
		return readstr(offset, buf, n, vgacard);
	case Qvgasize:
		sprint(obuf, "%dx%d%x%d %s",
			screeninfo.maxx, screeninfo.maxy,
			screeninfo.packed ? 16 : 256,
			screeninfo.interlaced ? "interlaced" : "non-interlaced");
		return readstr(offset, buf, n, obuf);
	case Qvgaportio:
		return 0;
	}
}

long
vgawrite(Chan *c, void *va, long n, ulong offset)
{
	if(offset != 0)
		error(Ebadarg);
	switch(c->qid.path&~CHDIR){
	case Qdir:
		error(Eperm);
	case Qvgamonitor:
	case Qvgatype:
	case Qvgasize:
	case Qvgaportio:
		return 0;
	}
}

void
vgaremove(Chan *c)
{
	error(Eperm);
}

void
vgawstat(Chan *c, char *dp)
{
	error(Eperm);
}
