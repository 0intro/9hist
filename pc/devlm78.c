#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

enum {
	Qdir,
	Qtemp,
};

static Dirtab lm78dir[] = {
	"temp",		{ Qtemp, 0 },		0,	0444,
};

SMBus *lm78smbus;

extern SMBus*	piix4smbus(void);

void
lm78init(void)
{
	lm78smbus = piix4smbus();
	if(lm78smbus != nil)
		print("found piix4 smbus, base %lud\n", lm78smbus->base);
}

static Chan*
lm78attach(char* spec)
{
	return devattach('T', spec);
}

int
lm78walk(Chan* c, char* name)
{
	return devwalk(c, name, lm78dir, nelem(lm78dir), devgen);
}

static void
lm78stat(Chan* c, char* dp)
{
	devstat(c, dp, lm78dir, nelem(lm78dir), devgen);
}

static Chan*
lm78open(Chan* c, int omode)
{
	return devopen(c, omode, lm78dir, nelem(lm78dir), devgen);
}

static void
lm78close(Chan*)
{
}

enum
{
	Linelen= 25,
};

static long
lm78read(Chan *c, void *a, long n, vlong offset)
{
	switch(c->qid.path & ~CHDIR){

	case Qdir:
		return devdirread(c, a, n, lm78dir, nelem(lm78dir), devgen);

	case Qtemp:
		error(Eperm);
	}
	return 0;
}

static long
lm78write(Chan *c, void *a, long n, vlong offset)
{
	error(Eperm);
	return 0;
}

Dev lm78devtab = {
	'T',
	"lm78",

	devreset,
	lm78init,
	lm78attach,
	devclone,
	lm78walk,
	lm78stat,
	lm78open,
	devcreate,
	lm78close,
	lm78read,
	devbread,
	lm78write,
	devbwrite,
	devremove,
	devwstat,
};
