#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/netif.h"
#include "../port/error.h"

#include	<libcrypt.h>

typedef struct Sdp Sdp;
typedef struct Conv Conv;
typedef struct Out Out;
typedef struct In In;
typedef struct ConPkt ConPkt;

enum
{
	Qtopdir=	1,		/* top level directory */

	Qsdpdir,			/* sdp directory */
	Qclone,
	Qstats,
	Qlog,

	Qconvdir,			/* directory per conversation */
	Qctl,
	Qdata,				/* unreliable packet channel */
	Qcontrol,			/* reliable control channel */
	Qstatus,

	MaxQ,

	Maxconv=	256,		// power of 2
	Nfs=		4,			// number of file systems
	Maxretries=	4,
};

#define TYPE(x) 	((x).path & 0xff)
#define CONV(x) 	(((x).path >> 8)&(Maxconv-1))
#define QID(x, y) 	(((x)<<8) | (y))

struct Out
{
	ulong	seqwrap;	// number of wraps of the sequence number
	ulong	seq;

	Block	*controlpkt;		// control channel
	ulong	*controlseq;

	void	*cipherstate;	// state cipher
	int		ivlen;			// in bytes
	int		(*encrypt)(Out*, uchar *buf, int len);

	void	*authstate;		// auth state
	int		authlen;		// auth data length in bytes
	int		(*auth)(Out*, uchar *buf, int len);

	void	*compstate;
	int		(*comp)(Out*, uchar *dst, uchar *src, int n);
};

struct In
{
	ulong	seqwrap;	// number of wraps of the sequence number
	ulong	seq;
	ulong	window;

	Block	*controlpkt;
	ulong	controlseq;

	void	*cipherstate;	// state cipher
	int		ivlen;			// in bytes
	int		(*decrypt)(In*, uchar *buf, int len);

	void	*authstate;		// auth state
	int		authlen;		// auth data length in bytes
	int		(*auth)(In*, uchar *buf, int len);

	void	*uncompstate;
	int		(*uncomp)(In*, uchar *dst, uchar *src, int n);
};

enum {
	CInit,
	COpening,
	COpen,
	CClosing,
	CClosed,
};

struct Conv {
	QLock;
	int	id;
	int ref;	// number of times the conv is opened
	Sdp	*sdp;

	int state;
	int dataopen;

	Proc *readproc;

	ulong	timeout;
	int		retries;

	// the following pair uniquely define conversation on this port
	ulong dialid;
	ulong acceptid;

	Chan *chan;	// packet channel

	char	user[NAMELEN];		/* protections */
	int	perm;

	int drop;

	In	in;
	Out	out;
};

struct Sdp {
	QLock;
	Log;
	Rendez	vous;			/* used by sdpackproc */
	int	nconv;
	Conv *conv[Maxconv];
	int ackproc;
};

enum {
	TConnect,
	TControl,
	TControlAck,
	TData,
	TThwackC,
	TThwackU,
};

enum {
	ConOpen,
	ConOpenAck,
	ConOpenNack,
	ConClose,
	ConCloseAck,
};

struct ConPkt
{
	uchar type;		// always zero = connection packet
	uchar op;
	uchar pad[2];
	uchar dialid[4];
	uchar acceptid[4];
};

static Dirtab sdpdirtab[]={
	"stats",	{Qstats},	0,	0444,
	"log",		{Qlog},		0,	0666,
	"clone",	{Qclone},		0,	0666,
};

static Dirtab convdirtab[]={
	"ctl",		{Qctl},	0,	0666,
	"data",		{Qdata},	0,	0666,
	"control",	{Qcontrol},	0,	0666,
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
static Conv *sdpclone(Sdp *sdp);
static void convsetstate(Conv *c, int state);
static void sendconnect(Conv *c, int op, ulong dialid, ulong acceptid);
static void sdpackproc(void *a);

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

	for(i=0; i<nelem(convdirtab); i++) {
		dt = convdirtab + i;
		dirtab[TYPE(dt->qid)] = dt;
	}

}

static Chan*
sdpattach(char* spec)
{
	Chan *c;
	int dev;
	char buf[100];
	Sdp *sdp;
	int start;

	dev = atoi(spec);
	if(dev<0 || dev >= Nfs)
		error("bad specification");

	c = devattach('T', spec);
	c->qid = (Qid){QID(0, Qtopdir)|CHDIR, 0};
	c->dev = dev;

	sdp = sdptab + dev;
	qlock(sdp);
	start = sdp->ackproc == 0;
	sdp->ackproc = 1;
	qunlock(sdp);

	if(start) {
		snprint(buf, sizeof(buf), "sdpackproc%d", dev);
		kproc(buf, sdpackproc, sdp);
	}
	
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
		case Qconvdir:
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
sdpopen(Chan* ch, int omode)
{
	int perm;
	Sdp *sdp;
	Conv *c;

	omode &= 3;
	perm = m2p[omode];
	USED(perm);

	sdp = sdptab + ch->dev;

	switch(TYPE(ch->qid)) {
	default:
		break;
	case Qtopdir:
	case Qsdpdir:
	case Qconvdir:
	case Qstats:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qlog:
		logopen(sdp);
		break;
	case Qclone:
		c = sdpclone(sdp);
		if(c == nil)
			error(Enodev);
		ch->qid.path = QID(c->id, Qctl);
		break;
	case Qdata:
	case Qctl:
	case Qstatus:
	case Qcontrol:
		c = sdp->conv[CONV(ch->qid)];
		qlock(c);
		if(waserror()) {
			qunlock(c);
			nexterror();
		}
		if((perm & (c->perm>>6)) != perm)
		if(strcmp(up->user, c->user) != 0 || (perm & c->perm) != perm)
				error(Eperm);
		c->ref++;
		qunlock(c);
		poperror();
		break;
	}
	ch->mode = openmode(omode);
	ch->flag |= COPEN;
	ch->offset = 0;
	return ch;
}

static void
sdpclose(Chan* ch)
{
	Sdp *sdp  = sdptab + ch->dev;

	switch(TYPE(ch->qid)) {
	case Qlog:
		if(ch->flag & COPEN)
			logclose(sdp);
		break;
	}
}

static long
sdpread(Chan *ch, void *a, long n, vlong off)
{
	char buf[256];
	Sdp *sdp = sdptab + ch->dev;
	char *s;
	Conv *c;

	USED(off);
	switch(TYPE(ch->qid)) {
	default:
		error(Eperm);
	case Qtopdir:
	case Qsdpdir:
	case Qconvdir:
		return devdirread(ch, a, n, 0, 0, sdpgen);
	case Qlog:
		return logread(sdp, a, off, n);
	case Qstatus:
		c = sdp->conv[CONV(ch->qid)];
		qlock(c);
		switch(c->state) {
		default:
			panic("unknown state");
		case CClosed:
			s = "closed";
			break;
		case COpening:
			s = "opening";
			break;
		case COpen:
			s = "open";
			break;
		case CClosing:
			s = "closing";
			break;
		}
		n = readstr(off, a, n, s);
		qunlock(c);
		return n;
	case Qctl:
		sprint(buf, "%lud", CONV(ch->qid));
		return readstr(off, a, n, buf);
	}
}

static long
sdpwrite(Chan *ch, void *a, long n, vlong off)
{
	Sdp *sdp = sdptab + ch->dev;
	Cmdbuf *cb;
	char *arg0;
	char *p;
	Conv *c;
	
	USED(off);
	switch(TYPE(ch->qid)) {
	default:
		error(Eperm);
	case Qctl:
		c = sdp->conv[CONV(ch->qid)];
print("Qctl write : conv->id = %d\n", c->id);
		cb = parsecmd(a, n);
		qlock(c);
		if(waserror()) {
			qunlock(c);
			free(cb);
			nexterror();
		}
		if(cb->nf == 0)
			error("short write");
		arg0 = cb->f[0];
print("cmd = %s\n", arg0);
		if(strcmp(arg0, "chan") == 0) {
			if(cb->nf != 2)
				error("usage: chan file");
			if(c->chan != nil)
				error("chan already set");
			c->chan = namec(cb->f[1], Aopen, ORDWR, 0);
		} else if(strcmp(arg0, "accept") == 0) {
			if(cb->nf != 2)
				error("usage: accect id");
			c->dialid = atoi(cb->f[1]);
			convsetstate(c, COpen);
		} else if(strcmp(arg0, "dial") == 0) {
			if(cb->nf != 1)
				error("usage: dial");
			convsetstate(c, COpening);
		} else if(strcmp(arg0, "drop") == 0) {
			if(cb->nf != 2)
				error("usage: drop permil");
			c->drop = atoi(cb->f[1]);
		} else
			error("unknown control request");
		poperror();
		qunlock(c);
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
		if(s >= sdp->nconv)
			return -1;
		qid = (Qid){QID(s,Qconvdir)|CHDIR, 0};
		snprint(buf, sizeof(buf), "%d", s);
		devdir(c, qid, buf, 0, eve, 0555, dp);
		return 1;
	case Qconvdir:
		if(s>=nelem(convdirtab))
			return -1;
		dt = convdirtab+s;
		qid = (Qid){QID(CONV(c->qid),TYPE(dt->qid)),0};
		devdir(c, qid, dt->name, dt->length, eve, dt->perm, dp);
		return 1;
	}
}

static Conv*
sdpclone(Sdp *sdp)
{
	Conv *c, **pp, **ep;

	c = nil;
	ep = sdp->conv + nelem(sdp->conv);
	qlock(sdp);
	if(waserror()) {
		qunlock(sdp);
		nexterror();
	}
	for(pp = sdp->conv; pp < ep; pp++) {
		c = *pp;
		if(c == nil){
			c = malloc(sizeof(Conv));
			if(c == nil)
				error(Enomem);
			qlock(c);
			c->sdp = sdp;
			c->id = pp - sdp->conv;
			*pp = c;
			sdp->nconv++;
			break;
		}
		if(canqlock(c)){
			if(c->state == CClosed)
				break;
			qunlock(c);
		}
	}
	poperror();
	qunlock(sdp);

	if(pp >= ep)
		return nil;

	c->ref++;
	c->state = CInit;

	strncpy(c->user, up->user, sizeof(c->user));
	c->perm = 0660;
	qunlock(c);

	return c;
}

// assume c is locked
static int
convretry(Conv *c)
{
	c->retries++;
	if(c->retries > Maxretries) {
print("convretry: giving up\n");
		convsetstate(c, CClosed);
		return 0;
	}
	c->timeout = TK2SEC(m->ticks) + (1<<c->retries);
	return 1;
}

static void
convtimer(Conv *c, ulong sec)
{
	if(c->timeout == 0 || c->timeout > sec)
		return;
	qlock(c);
	if(waserror()) {
		qunlock(c);
		nexterror();
	}
	switch(c->state) {
	case COpening:
print("COpening timeout\n");
		if(convretry(c))
			sendconnect(c, ConOpen, c->dialid, 0);
		break;
	case COpen:
		// check for control packet
		break;
	case CClosing:
print("CClosing timeout\n");
		if(convretry(c))
			sendconnect(c, ConClose, c->dialid, c->acceptid);
		break;
	}
	qunlock(c);
}


static void
sdpackproc(void *a)
{
	Sdp *sdp = a;
	ulong sec;
	int i;
	Conv *c;

	for(;;) {
		tsleep(&sdp->vous, return0, 0, 1000);
		sec = TK2SEC(m->ticks);
		qlock(sdp);
		for(i=0; i<sdp->nconv; i++) {
			c = sdp->conv[i];
			if(!waserror()) {
				convtimer(c, sec);
				poperror();
			}
		}
		qunlock(sdp);
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

// assume hold lock on c
static void
convsetstate(Conv *c, int state)
{
	switch(state) {
	default:
		panic("setstate: bad state: %d", state);
	case COpening:
		if(c->state != CInit)
			error("convsetstate: illegal transition");
		c->dialid = (rand()<<16) + rand();
		c->timeout = TK2SEC(m->ticks) + 2;
		c->retries = 0;
		sendconnect(c, ConOpen, c->dialid, 0);
		break;
	case COpen:
		switch(c->state) {
		default:
			error("convsetstate: illegal transition");
		case CInit:
			c->acceptid = (rand()<<16) + rand();
			sendconnect(c, ConOpenAck, c->dialid, c->acceptid);
			break;
		case COpening:
			break;
		}
		// setup initial key and auth method
		break;
	case CClosing:
		c->timeout = TK2SEC(m->ticks) + 2;
		c->retries = 0;
		break;
	case CClosed:
		break;
	}
	c->state = state;
}

static void
sendconnect(Conv *c, int op, ulong dialid, ulong acceptid)
{
	ConPkt con;

	if(c->chan == nil) {
print("chan = nil\n");
		error("no channel attached");
	}
	memset(&con, 0, sizeof(con));
	con.type = TConnect;
	con.op = op;
	hnputl(con.dialid, dialid);
	hnputl(con.acceptid, acceptid);

	// simulated errors
	if(c->drop && c->drop > nrand(c->drop))
		return;
	devtab[c->chan->type]->write(c->chan, &con, sizeof(con), 0);
}
