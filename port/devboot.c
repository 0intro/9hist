#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

enum{
	Qdir,
	Qboot,
	Qmem,
};

Dirtab bootdir[]={
	"boot",		Qboot,		0,			0666,
	"mem",		Qmem,		0,			0666,
};

#define	NBOOT	(sizeof bootdir/sizeof(Dirtab))

void
bootreset(void)
{
}

void
bootinit(void)
{
}

Chan*
bootattach(char *spec)
{
	return devattach('b', spec);
}

Chan*
bootclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
bootwalk(Chan *c, char *name)
{
	return devwalk(c, name, bootdir, NBOOT, devgen);
}

void	 
bootstat(Chan *c, char *dp)
{
	devstat(c, dp, bootdir, NBOOT, devgen);
}

Chan*
bootopen(Chan *c, int omode)
{
	return devopen(c, omode, bootdir, NBOOT, devgen);
}

void	 
bootcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

/*
 * sysremove() knows this is a nop
 */
void	 
bootclose(Chan *c)
{
}

long	 
bootread(Chan *c, void *buf, long n)
{
	switch(c->qid & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, bootdir, NBOOT, devgen);
	}

	error(0, Egreg);
}

long	 
bootwrite(Chan *c, void *buf, long n)
{
	ulong pc;

	switch(c->qid & ~CHDIR){
	case Qmem:
		/* kernel memory.  BUG: shouldn't be so easygoing. BUG: mem mapping? */
		if(c->offset>=KZERO && c->offset<KZERO+conf.npage*BY2PG){
/*			print("%ux, %d\n", c->offset, n);/**/
			if(c->offset+n > KZERO+conf.npage*BY2PG)
				n = KZERO+conf.npage*BY2PG - c->offset;
			memcpy((char*)c->offset, buf, n);
			return n;
		}
		error(0, Ebadarg);

	case Qboot:
		pc = *(ulong*)buf;
		splhi();
		gotopc(pc);
	}
	error(0, Ebadarg);
}

void	 
bootremove(Chan *c)
{
	error(0, Eperm);
}

void	 
bootwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
bootuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
booterrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}
