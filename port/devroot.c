#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

enum{
	Qdir,
	Qbin,
	Qboot,
	Qcfs,
	Qdev,
	Qenv,
	Qkfs,
	Qproc,
};

extern long	cfslen;
extern ulong	cfscode[];
extern long	kfslen;
extern ulong	kfscode[];

Dirtab rootdir[]={
	"bin",		{Qbin|CHDIR},	0,			0700,
	"boot",		{Qboot},	0,			0700,
	"dev",		{Qdev|CHDIR},	0,			0700,
	"env",		{Qenv|CHDIR},	0,			0700,
	"proc",		{Qproc|CHDIR},	0,			0700,
};
#define	NROOT	(sizeof rootdir/sizeof(Dirtab))
Dirtab rootpdir[]={
	"cfs",		{Qcfs},		0,			0700,
	"kfs",		{Qkfs},		0,			0700,
};
Dirtab *rootmap[sizeof rootpdir/sizeof(Dirtab)];
int	nroot;

int
rootgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	if(tab==0 || i>=ntab)
		return -1;
	if(i < NROOT)
		tab += i;
	else
		tab = rootmap[i - NROOT];
	devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
	return 1;
}

void
rootreset(void)
{
	int i;

	i = 0;
	if(cfslen)
		rootmap[i++] = &rootpdir[0];
	if(kfslen)
		rootmap[i++] = &rootpdir[1];
	nroot = NROOT + i;
}

void
rootinit(void)
{
}

Chan*
rootattach(char *spec)
{
	return devattach('/', spec);
}

Chan*
rootclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
rootwalk(Chan *c, char *name)
{
	return devwalk(c, name, rootdir, nroot, rootgen);
}

void	 
rootstat(Chan *c, char *dp)
{
	devstat(c, dp, rootdir, nroot, rootgen);
}

Chan*
rootopen(Chan *c, int omode)
{
	return devopen(c, omode, rootdir, nroot, rootgen);
}

void	 
rootcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

/*
 * sysremove() knows this is a nop
 */
void	 
rootclose(Chan *c)
{
}

#include	"boot.h"

long	 
rootread(Chan *c, void *buf, long n, ulong offset)
{

	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, rootdir, nroot, rootgen);

	case Qboot:		/* boot */
		if(offset >= sizeof bootcode)
			return 0;
		if(offset+n > sizeof bootcode)
			n = sizeof bootcode - offset;
		memmove(buf, ((char*)bootcode)+offset, n);
		return n;

	case Qcfs:		/* cfs */
		if(offset >= cfslen)
			return 0;
		if(offset+n > cfslen)
			n = cfslen - offset;
		memmove(buf, ((char*)cfscode)+offset, n);
		return n;

	case Qkfs:		/* kfs */
		if(offset >= kfslen)
			return 0;
		if(offset+n > kfslen)
			n = kfslen - offset;
		memmove(buf, ((char*)kfscode)+offset, n);
		return n;

	case Qdev:
		return 0;
	}
	error(Egreg);
	return 0;
}

long	 
rootwrite(Chan *c, void *buf, long n, ulong offset)
{
	error(Egreg);
}

void	 
rootremove(Chan *c)
{
	error(Eperm);
}

void	 
rootwstat(Chan *c, char *dp)
{
	error(Eperm);
}
