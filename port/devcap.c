#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"netif.h"
#include	<libsec.h>

enum
{
	Hashlen=	SHA1dlen,
	Maxhash=	256,
};

/*
 *  if a process knows cap->cap, it can change user
 *  to capabilty->user.
 */
typedef struct Caphash	Caphash;
struct Caphash
{
	Caphash	*next;
	char		hash[Hashlen];
	ulong		ticks;
};

struct
{
	QLock;
	int	opens;
	Caphash	*first;
	int	nhash;
} capalloc;

enum
{
	Qdir,
	Qhash,
	Quse,
};

Dirtab capdir[] =
{
	".",		{Qdir,0,QTDIR},	0,		DMDIR|0500,
	"caphash",	{Qhash},	0,		0200,
	"capuse",	{Quse},		0,		0222,
};
#define NCAPDIR 3

static Chan*
capattach(char *spec)
{
	return devattach(L'¤', spec);
}

static Walkqid*
capwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, capdir, NCAPDIR, devgen);
}

static int
capstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, capdir, NCAPDIR, devgen);
}

/*
 *  if the stream doesn't exist, create it
 */
static Chan*
capopen(Chan *c, int omode)
{
	if(c->qid.type & QTDIR){
		if(omode != OREAD)
			error(Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	if(waserror()){
		qunlock(&capalloc);
		nexterror();
	}
	qlock(&capalloc);
	switch((ulong)c->qid.path){
	case Qhash:
		if(capalloc.opens > 0){
			qunlock(&capalloc);
			error("exclusive use");
		}
		capalloc.opens++;
		break;
	}
	poperror();
	qunlock(&capalloc);

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static Caphash*
remcap(uchar *hash)
{
	Caphash *t, **l;

	qlock(&capalloc);

	/* find the matching capability */
	for(l = &capalloc.first; *l != nil;){
		t = *l;
		if(memcmp(hash, t->hash, Hashlen) == 0)
			break;
		l = &t->next;
	}
	t = *l;
	if(t != nil){
		capalloc.nhash--;
		*l = t->next;
	}
	qunlock(&capalloc);

	return t;
}

/* add a capability, throwing out any old ones */
static void
addcap(uchar *hash)
{
	Caphash *p, *t, **l;

	p = smalloc(sizeof *p);
	memmove(p->hash, hash, Hashlen);
	p->next = nil;
	p->ticks = m->ticks;

	qlock(&capalloc);

	/* trim extras */
	while(capalloc.nhash >= Maxhash){
		t = capalloc.first;
		if(t == nil)
			panic("addcap");
		capalloc.first = t->next;
		free(t);
		capalloc.nhash--;
	}

	/* add new one */
	for(l = &capalloc.first; *l != nil; l = &(*l)->next)
		;
	*l = p;
	capalloc.nhash++;

	qunlock(&capalloc);
}

static void
capclose(Chan *c)
{
	if(c->flag & COPEN)
		if(c->qid.path == Qhash){
			qlock(&capalloc);
			capalloc.opens--;
			qunlock(&capalloc);
		}
}

static long
capread(Chan *c, void *va, long n, vlong)
{
	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, va, n, capdir, nelem(capdir), devgen);

	default:
		error(Eperm);
		break;
	}
	return n;
}

static long
capwrite(Chan *c, void *va, long n, vlong)
{
	Caphash *p;
	char *cp;
	uchar hash[Hashlen];
	char *key, *user;
	char err[256];

	switch((ulong)c->qid.path){
	case Qhash:
		if(n < Hashlen)
			error(Eshort);
		memmove(hash, va, Hashlen);
		addcap(hash);
		break;

	case Quse:
		/* copy key to avoid a fault in hmac_xx */
		cp = nil;
		if(waserror()){
			free(cp);
			nexterror();
		}
		cp = smalloc(n+1);
		memmove(cp, va, n);
		cp[n] = 0;

		user = cp;
		key = strchr(cp, '@');
		if(key == nil)
			error(Eshort);
		*key++ = 0;

		hmac_sha1((uchar*)user, strlen(user), (uchar*)key, strlen(key), hash, nil);

		p = remcap(hash);
		if(p == nil){
			snprint(err, sizeof err, "invalid capability %s@%s", user, key);
			error(err);
		}

		/* set user id */
		kstrdup(&up->user, user);
		up->basepri = PriNormal;

		free(p);
		free(cp);
		poperror();
		break;

	default:
		error(Eperm);
		break;
	}

	return n;
}

Dev capdevtab = {
	L'¤',
	"cap",

	devreset,
	devinit,
	devshutdown,
	capattach,
	capwalk,
	capstat,
	capopen,
	devcreate,
	capclose,
	capread,
	devbread,
	capwrite,
	devbwrite,
	devremove,
	devwstat,
};
