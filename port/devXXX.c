/*
 *  template for making a new device
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"


enum{
	Qdir,
	Qdata,
};
Dirtab XXXtab[]={
	"data",		{Qdata, 0},	0,	0600,
};

static void
XXXreset(void)						/* default in dev.c */
{
}

static void
XXXinit(void)						/* default in dev.c */
{
}

static Chan*
XXXattach(char* spec)
{
	return devattach('X', spec);
}

static Chan*
XXXclone(Chan* c, Chan* nc)				/* default in dev.c */
{
	return devclone(c, nc);
}

static int
XXXwalk(Chan* c, char* name)
{
	return devwalk(c, name, XXXtab, nelem(XXXtab), devgen);
}

static void
XXXstat(Chan* c, char* db)
{
	devstat(c, db, XXXtab, nelem(XXXtab), devgen);
}

static Chan*
XXXopen(Chan* c, int omode)
{
	return devopen(c, omode, XXXtab, nelem(XXXtab), devgen);
}

static void
XXXcreate(Chan* c, char* name, int omode, ulong perm)	/* default in dev.c */
{
	USED(c, name, omode, perm);
	error(Eperm);
}

static void
XXXremove(Chan* c)					/* default in dev.c */
{
	USED(c);
	error(Eperm);
}

static void
XXXwstat(Chan* c, char* dp)				/* default in dev.c */
{
	USED(c, dp);
	error(Eperm);
}

static void
XXXclose(Chan* c)
{
	USED(c);
}

static long
XXXread(Chan* c, void* a, long n, ulong offset)
{
	USED(offset);

	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, XXXtab, nelem(XXXtab), devgen);
	case Qdata:
		break;
	default:
		n=0;
		break;
	}
	return n;
}

static Block*
XXXbread(Chan* c, long n, ulong offset)			/* default in dev.c */
{
	return devbread(c, n, offset);
}

static long
XXXwrite(Chan* c, char* a, long n, ulong offset)
{
	USED(a, offset);

	switch(c->qid.path & ~CHDIR){
	case Qdata:
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

static long
XXXbwrite(Chan* c, Block* bp, ulong offset)		/* default in dev.c */
{
	return devbwrite(c, bp, offset);
}

Dev XXXdevtab = {					/* defaults in dev.c */
	'X',
	"XXX",

	XXXreset,					/* devreset */
	XXXinit,					/* devinit */
	XXXattach,
	XXXclone,					/* devclone */
	XXXwalk,
	XXXstat,
	XXXopen,
	XXXcreate,					/* devcreate */
	XXXclose,
	XXXread,
	XXXbread,					/* devbread */
	XXXwrite,
	XXXbwrite,					/* devbwrite */
	XXXremove,					/* devremove */
	XXXwstat,					/* devwstat */
};
