#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/netif.h"
#include "../port/error.h"

#include	<libcrypt.h>

typedef struct Sdp		Sdp;
typedef struct Port 	Port;

enum
{
	Qtopdir=	1,		/* top level directory */

	Qsdpdir,			/* sdp directory */
	Qclone,
	Qstats,
	Qlog,

	Qportdir,			/* directory for a protocol */
	Qctl,
	Qdata,				/* reliable control channel */
	Qpacket,			/* unreliable packet channel */
	Qerr,
	Qlisten,
	Qlocal,
	Qremote,
	Qstatus,

	MaxQ,

	Maxport=	256,		// power of 2
	Nfs=		4,			// number of file systems
};

#define TYPE(x) 	((x).path & 0xff)
#define PORT(x) 	(((x).path >> 8)&(Maxport-1))
#define QID(x, y) 	(((x)<<8) | (y))

struct Port {
	int	id;
	Sdp	*sdp;
	int	ref;
	int	closed;
};

struct Sdp {
	QLock;
	Log;
	int	nport;
	Port	*port[Maxport];
};

static Dirtab sdpdirtab[]={
	"ctl",		{Qctl},	0,	0666,
	"stats",	{Qstats},	0,	0444,
	"log",		{Qlog},		0,	0666,
};

static Dirtab portdirtab[]={
	"ctl",		{Qctl},	0,	0666,
	"data",		{Qdata},	0,	0666,
	"packet",	{Qpacket},	0,	0666,
	"listen",	{Qlisten},	0,	0666,
	"local",	{Qlocal},	0,	0444,
	"remote",	{Qlocal},	0,	0444,
	"status",	{Qstatus},	0,	0444,
};

static int m2p[] = {
	[OREAD]		4,
	[OWRITE]	2,
	[ORDWR]		6
};

enum {
	Logcompress=	(1<<0),
	Logauth=	(1<<1),
	Loghmac=	(1<<2),
};

static Logflag logflags[] =
{
	{ "compress",	Logcompress, },
	{ "auth",	Logauth, },
	{ "hmac",	Loghmac, },
	{ nil,		0, },
};

static Dirtab	*dirtab[MaxQ];
static Sdp sdptab[Nfs];

static int sdpgen(Chan *c, Dirtab*, int, int s, Dir *dp);

static void
sdpinit(void)
{
	int i;
	Dirtab *dt;

	// setup dirtab with non directory entries
	for(i=0; i<nelem(sdpdirtab); i++) {
		dt = sdpdirtab + i;
		dirtab[TYPE(dt->qid)] = dt;
	}

	for(i=0; i<nelem(portdirtab); i++) {
		dt = portdirtab + i;
		dirtab[TYPE(dt->qid)] = dt;
	}
}

static Chan*
sdpattach(char* spec)
{
	Chan *c;
	int dev;

	dev = atoi(spec);
	if(dev<0 || dev >= Nfs)
		error("bad specification");

	c = devattach('B', spec);
	c->qid = (Qid){QID(0, Qtopdir)|CHDIR, 0};
	c->dev = dev;

	return c;
}

static int
sdpwalk(Chan *c, char *name)
{
	if(strcmp(name, "..") == 0){
		switch(TYPE(c->qid)){
		case Qtopdir:
		case Qsdpdir:
			c->qid = (Qid){CHDIR|Qtopdir, 0};
			break;
		case Qportdir:
			c->qid = (Qid){CHDIR|Qsdpdir, 0};
			break;
		default:
			panic("sdpwalk %lux", c->qid.path);
		}
		return 1;
	}

	return devwalk(c, name, 0, 0, sdpgen);
}

static void
sdpstat(Chan* c, char* db)
{
	devstat(c, db, nil, 0, sdpgen);
}

static Chan*
sdpopen(Chan* c, int omode)
{
	int perm;
	Sdp *sdp;

	omode &= 3;
	perm = m2p[omode];
	USED(perm);

	sdp = sdptab + c->dev;

	switch(TYPE(c->qid)) {
	default:
		break;
	case Qtopdir:
	case Qsdpdir:
	case Qportdir:
	case Qstatus:
	case Qlocal:
	case Qstats:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qlog:
		logopen(sdp);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
sdpclose(Chan* c)
{
	Sdp *sdp  = sdptab + c->dev;

	switch(TYPE(c->qid)) {
	case Qlog:
		if(c->flag & COPEN)
			logclose(sdp);
		break;
	}
}

static long
sdpread(Chan *c, void *a, long n, vlong off)
{
	char buf[256];
	Sdp *sdp = sdptab + c->dev;
	Port *port;

	USED(off);
	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qtopdir:
	case Qsdpdir:
	case Qportdir:
		return devdirread(c, a, n, 0, 0, sdpgen);
	case Qlog:
		return logread(sdp, a, off, n);
	case Qstatus:
		qlock(sdp);
		port = sdp->port[PORT(c->qid)];
		if(port == 0)
			strcpy(buf, "unbound\n");
		else {
		}
		n = readstr(off, a, n, buf);
		qunlock(sdp);
		return n;
	}

}

static long
sdpwrite(Chan *c, void *a, long n, vlong off)
{
	Sdp *sdp = sdptab + c->dev;
	Cmdbuf *cb;
	char *arg0;
	char *p;
	
	USED(off);
	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qctl:
		cb = parsecmd(a, n);
		qlock(sdp);
		if(waserror()) {
			qunlock(sdp);
			free(cb);
			nexterror();
		}
		if(cb->nf == 0)
			error("short write");
		arg0 = cb->f[0];
		if(strcmp(arg0, "xxx") == 0) {
			print("xxx\n");
		} else
			error("unknown control request");
		poperror();
		qunlock(sdp);
		free(cb);
		return n;
	case Qlog:
		cb = parsecmd(a, n);
		p = logctl(sdp, cb->nf, cb->f, logflags);
		free(cb);
		if(p != nil)
			error(p);
		return n;
	}
}

static int
sdpgen(Chan *c, Dirtab*, int, int s, Dir *dp)
{
	Sdp *sdp = sdptab + c->dev;
	int type = TYPE(c->qid);
	char buf[32];
	Dirtab *dt;
	Qid qid;

	switch(type) {
	default:
		// non directory entries end up here
		if(c->qid.path & CHDIR)
			panic("sdpgen: unexpected directory");	
		if(s != 0)
			return -1;
		dt = dirtab[TYPE(c->qid)];
		if(dt == nil)
			panic("sdpgen: unknown type: %d", TYPE(c->qid));
		devdir(c, c->qid, dt->name, dt->length, eve, dt->perm, dp);
		return 1;
	case Qtopdir:
		if(s != 0)
			return -1;
		devdir(c, (Qid){QID(0,Qsdpdir)|CHDIR,0}, "sdp", 0, eve, 0555, dp);
		return 1;
	case Qsdpdir:
		if(s<nelem(sdpdirtab)) {
			dt = sdpdirtab+s;
			devdir(c, dt->qid, dt->name, dt->length, eve, dt->perm, dp);
			return 1;
		}
		s -= nelem(sdpdirtab);
		if(s >= sdp->nport)
			return -1;
		qid = (Qid){QID(s,Qportdir)|CHDIR, 0};
		snprint(buf, sizeof(buf), "%d", s);
		devdir(c, qid, buf, 0, eve, 0555, dp);
		return 1;
	case Qportdir:
		if(s>=nelem(portdirtab))
			return -1;
		dt = portdirtab+s;
		qid = (Qid){QID(PORT(c->qid),TYPE(dt->qid)),0};
		devdir(c, qid, dt->name, dt->length, eve, dt->perm, dp);
		return 1;
	}
}


Dev sdpdevtab = {
	'T',
	"sdp",

	devreset,
	sdpinit,
	sdpattach,
	devclone,
	sdpwalk,
	sdpstat,
	sdpopen,
	devcreate,
	sdpclose,
	sdpread,
	devbread,
	sdpwrite,
	devbwrite,
	devremove,
	devwstat,
};
