#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum{
	Qdir=	0,
	Qbin,
	Qdev,
	Qenv,
	Qproc,
	Qnet,
	Qnetalt,
	Qrecover,
	Qboot,		/* readable files */

	Nfiles=13,	/* max root files */	
};

extern ulong	bootlen;
extern uchar	bootcode[];

Dirtab rootdir[Nfiles]={
	"bin",		{Qbin|CHDIR},	0,	0777,
	"dev",		{Qdev|CHDIR},	0,	0777,
	"env",		{Qenv|CHDIR},	0,	0777,
	"proc",		{Qproc|CHDIR},	0,	0777,
	"net",		{Qnet|CHDIR},	0,	0777,
	"net.alt",	{Qnetalt|CHDIR},	0,	0777,
	"recover",	{Qrecover},	0,	0777,
};

static uchar	*rootdata[Nfiles];
static int	nroot = Qboot - 1;
static int	recovbusy;

typedef struct Recover Recover;
struct Recover
{
	int	len;
	char	*req;
	Recover	*next;
};

struct 
{
	Lock;
	QLock;
	Rendez;
	Recover	*q;
}reclist;

/*
 *  add a root file
 */
void
addrootfile(char *name, uchar *contents, ulong len)
{
	Dirtab *d;
	

	if(nroot >= Nfiles)
		panic("too many root files");
	rootdata[nroot] = contents;
	d = &rootdir[nroot];
	strcpy(d->name, name);
	d->length = len;
	d->perm = 0555;
	d->qid.path = nroot+1;
	nroot++;
}

void
rootreset(void)
{
	addrootfile("boot", bootcode, bootlen);	/* always have a boot file */
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
	if(strcmp(name, "..") == 0) {
		c->qid.path = Qdir|CHDIR;
		return 1;
	}
	if((c->qid.path & ~CHDIR) != Qdir)
		return 0;
	return devwalk(c, name, rootdir, nroot, devgen);
}

void	 
rootstat(Chan *c, char *dp)
{
	devstat(c, dp, rootdir, nroot, devgen);
}

Chan*
rootopen(Chan *c, int omode)
{
	switch(c->qid.path & ~CHDIR) {
	default:
		break;
	case Qrecover:
		if(recovbusy)
			error(Einuse);		
		if(strcmp(up->user, eve) != 0)
			error(Eperm);
		recovbusy = 1;
		break;
	}

	return devopen(c, omode, rootdir, nroot, devgen);
}

void	 
rootcreate(Chan*, char*, int, ulong)
{
	error(Eperm);
}

/*
 * sysremove() knows this is a nop
 */
void	 
rootclose(Chan *c)
{
	switch(c->qid.path) {
	default:
		break;
	case Qrecover:
		if(c->flag&COPEN)
			recovbusy = 0;
		break;
	}
}

int
rdrdy(void*)
{
	return reclist.q != 0;
}

long	 
rootread(Chan *c, void *buf, long n, ulong offset)
{
	ulong t;
	Dirtab *d;
	uchar *data;
	Recover *r;

	t = c->qid.path & ~CHDIR;
	switch(t){
	case Qdir:
		return devdirread(c, buf, n, rootdir, nroot, devgen);
	case Qrecover:
		qlock(&reclist);
		if(waserror()) {
			qunlock(&reclist);
			nexterror();
		}

		sleep(&reclist, rdrdy, 0);

		lock(&reclist);
		r = reclist.q;
		reclist.q = r->next;
		unlock(&reclist);

		qunlock(&reclist);

		poperror();
		if(n < r->len)
			n = r->len;
		memmove(buf, r->req, n);
		free(r->req);
		free(r);
		return n;
	}

	if(t < Qboot)
		return 0;

	d = &rootdir[t-1];
	data = rootdata[t-1];
	if(offset >= d->length)
		return 0;
	if(offset+n > d->length)
		n = d->length - offset;
	memmove(buf, data+offset, n);
	return n;
}

Block*
rootbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long	 
rootwrite(Chan *c, void *buf, long n, ulong)
{
	char tmp[256];

	switch(c->qid.path & ~CHDIR){
	default:
		error(Egreg);
	case Qrecover:
		if(n > sizeof(tmp)-1)
			error(Etoosmall);
		/* Nul terminate */
		memmove(tmp, buf, n);
		tmp[n] = '\0';
		mntrepl(tmp);
		return n;
	}
	return 0;
}

long
rootbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void	 
rootremove(Chan*)
{
	error(Eperm);
}

void	 
rootwstat(Chan*, char*)
{
	error(Eperm);
}

void
rootrecover(Path *p, char *mntname)
{
	int i;
	Recover *r;
	char buf[256];

	r = malloc(sizeof(Recover));
	i = ptpath(p, buf, sizeof(buf));
	r->req = smalloc(i+strlen(mntname)+2);
	sprint(r->req, "%s %s", buf, mntname);
	lock(&reclist);
	r->next = reclist.q;
	reclist.q = r;
	unlock(&reclist);
	wakeup(&reclist);
}
