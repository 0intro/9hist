#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/netif.h"
#include "../port/error.h"

#include	<libcrypt.h>
#include "../port/thwack.h"

/*
 * sdp - secure datagram protocol
 */

typedef struct Sdp Sdp;
typedef struct Conv Conv;
typedef struct OneWay OneWay;
typedef struct Stats Stats;
typedef struct ConnectPkt ConnectPkt;
typedef struct AckPkt AckPkt;
typedef struct Algorithm Algorithm;

enum
{
	Qtopdir=	1,		/* top level directory */

	Qsdpdir,			/* sdp directory */
	Qclone,
	Qlog,

	Qconvdir,			/* directory per conversation */
	Qctl,
	Qdata,				/* unreliable packet channel */
	Qcontrol,			/* reliable control channel */
	Qstatus,
	Qstats,
	Qrstats,

	MaxQ,

	Maxconv= 256,		// power of 2
	Nfs= 4,				// number of file systems
	MaxRetries=	8,
	KeepAlive = 60,		// keep alive in seconds
	KeyLength= 32,
	SeqMax = (1<<24),
	SeqWindow = 32,
};

#define TYPE(x) 	((x).path & 0xff)
#define CONV(x) 	(((x).path >> 8)&(Maxconv-1))
#define QID(x, y) 	(((x)<<8) | (y))

struct Stats
{
	ulong	outPackets;
	ulong	outDataPackets;
	ulong	outDataBytes;
	ulong	outCompDataBytes;
	ulong	outCompBytes;
	ulong	inPackets;
	ulong	inDataPackets;
	ulong	inDataBytes;
	ulong	inCompDataBytes;
	ulong	inMissing;
	ulong	inDup;
	ulong	inReorder;
	ulong	inBadAuth;
	ulong	inBadSeq;
};

struct OneWay
{
	Rendez	statsready;

	ulong	seqwrap;	// number of wraps of the sequence number
	ulong	seq;
	ulong	window;

	QLock	controllk;
	Rendez	controlready;
	Block	*controlpkt;		// control channel
	ulong	controlseq;

	void	*cipherstate;	// state cipher
	int		cipherivlen;	// initial vector length
	int		cipherblklen;	// block length
	int		(*cipher)(OneWay*, uchar *buf, int len);

	void	*authstate;		// auth state
	int		authlen;		// auth data length in bytes
	int		(*auth)(OneWay*, uchar *buf, int len);

	void	*compstate;
	int		(*comp)(OneWay*, uchar *dst, uchar *src, int n);
};

// conv states
enum {
	CInit,
	CDial,
	CAccept,
	COpen,
	CLocalClose,
	CRemoteClose,
	CClosed,
};

struct Conv {
	QLock;
	int	id;
	int ref;	// number of times the conv is opened
	Sdp	*sdp;

	int state;
	int dataopen;
	int reader;		// reader proc has been started

	Stats	lstats;
	Stats	rstats;
	
	ulong	lastrecv;	// time last packet was received 
	ulong	timeout;
	int		retries;

	// the following pair uniquely define conversation on this port
	ulong dialid;
	ulong acceptid;

	QLock readlk;		// protects readproc
	Proc *readproc;

	Chan *chan;	// packet channel
	char *channame;

	char owner[NAMELEN];		/* protections */
	int	perm;

	uchar	masterkey[KeyLength];
	char *authname;
	char *ciphername;
	char *compname;

	int drop;

	OneWay	in;
	OneWay	out;
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
	ConOpenRequest,
	ConOpenAck,
	ConOpenAckAck,
	ConClose,
	ConReset,
};

struct ConnectPkt
{
	uchar type;		// always zero = connection packet
	uchar op;
	uchar pad[2];
	uchar dialid[4];
	uchar acceptid[4];
};

struct AckPkt
{
	uchar	cseq[4];
	uchar	outPackets[4];
	uchar	outDataPackets[4];
	uchar	outDataBytes[4];
	uchar	outCompDataBytes[4];
	uchar	inPackets[4];
	uchar	inDataPackets[4];
	uchar	inDataBytes[4];
	uchar	inCompDataBytes[4];
	uchar	inMissing[4];
	uchar	inDup[4];
	uchar	inReorder[4];
	uchar	inBadAuth[4];
	uchar	inBadSeq[4];
};

struct Algorithm
{
	char 	*name;
	int		keylen;		// in bytes
	void	(*init)(Conv*, char* name, int keylen);
};

static Dirtab sdpdirtab[]={
	"log",		{Qlog},		0,	0666,
	"clone",	{Qclone},		0,	0666,
};

static Dirtab convdirtab[]={
	"ctl",		{Qctl},	0,	0666,
	"data",		{Qdata},	0,	0666,
	"control",	{Qcontrol},	0,	0666,
	"status",	{Qstatus},	0,	0444,
	"stats",	{Qstats},	0,	0444,
	"rstats",	{Qrstats},	0,	0444,
};

#ifdef XXX
static Algorithm cipheralg[] =
{
	"null",			0,	nullcipherinit,
	"des_56_cbc",	7,	descipherinit,
	"rc4_128",		16,	rc4cipherinit,
	nil,			0,	nil,
};

static Algorithm authalg[] =
{
	"null",			0,	nullauthinit,
	"hmac_sha_96",	16,	shaauthinit,
	"hmac_md5_96",	16,	md5authinit,
	nil,			0,	nil,
};

static Algorithm compalg[] =
{
	"null",			0,	nullcompinit,
	"thwack",		0,	thwackcompinit,
	nil,			0,	nil,
};
#endif

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
static char *convstatename[] = {
	[CInit] "Init",
	[CDial] "Dial",
	[CAccept] "Accept",
	[COpen] "Open",
	[CLocalClose] "LocalClose",
	[CRemoteClose] "RemoteClose",
	[CClosed] "Closed",
};

static int sdpgen(Chan *c, Dirtab*, int, int s, Dir *dp);
static Conv *sdpclone(Sdp *sdp);
static void convsetstate(Conv *c, int state);
static void sdpackproc(void *a);
static void onewaycleanup(OneWay *ow);
static int readready(void *a);
static int controlread();
static Block *conviput(Conv *c, Block *b, int control);
static void conviput2(Conv *c, Block *b);
static void writecontrol(Conv *c, void *p, int n, int wait);
static Block *readcontrol(Conv *c, int n);
static Block *readdata(Conv *c, int n);
static long writedata(Conv *c, Block *b);
static void convoput(Conv *c, int type, Block *b);
static void convoput2(Conv *c, int op, ulong dialid, ulong acceptid);
static void convreader(void *a);
static void convopenchan(Conv *c, char *path);
static void convstats(Conv *c, int local, char *buf, int n);

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
	case Qstats:
	case Qrstats:
		c = sdp->conv[CONV(ch->qid)];
		qlock(c);
		if(waserror()) {
			qunlock(c);
			nexterror();
		}
		if((perm & (c->perm>>6)) != perm)
		if(strcmp(up->user, c->owner) != 0 || (perm & c->perm) != perm)
				error(Eperm);
		c->ref++;
		if(TYPE(ch->qid) == Qdata) {
			if(c->dataopen == 0)
			if(c->readproc != nil)
				postnote(c->readproc, 1, "interrupt", 0);
			c->dataopen++;
		}
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
	Conv *c;

	switch(TYPE(ch->qid)) {
	case Qlog:
		if(ch->flag & COPEN)
			logclose(sdp);
		break;
	case Qdata:
	case Qctl:
	case Qstatus:
	case Qcontrol:
		if(!(ch->flag & COPEN))
			break;
		c = sdp->conv[CONV(ch->qid)];
		qlock(c);
		if(waserror()) {
			qunlock(c);
			nexterror();
		}
		c->ref--;
		if(TYPE(ch->qid) == Qdata) {
			c->dataopen--;
			if(c->dataopen == 0 && c->reader == 0) {
				kproc("convreader", convreader, c);
				c->reader = 1;
			}
		}
		if(c->ref == 0) {
			switch(c->state) {
			default:
				convsetstate(c, CClosed);
				break;
			case CAccept:
			case COpen:
				convsetstate(c, CLocalClose);
				break;
			case CLocalClose:
				panic("local close already happened");
			}
		}
		qunlock(c);
		poperror();
		break;
	}
}

static long
sdpread(Chan *ch, void *a, long n, vlong off)
{
	char buf[256];
	char *s;
	Sdp *sdp = sdptab + ch->dev;
	Conv *c;
	Block *b;
	int rv;

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
		n = readstr(off, a, n, convstatename[c->state]);
		qunlock(c);
		return n;
	case Qctl:
		sprint(buf, "%lud", CONV(ch->qid));
		return readstr(off, a, n, buf);
	case Qcontrol:
		b = readcontrol(sdp->conv[CONV(ch->qid)], n);
		if(b == nil)
			return 0;
print("readcontrol asked %ld got %ld\n", n, BLEN(b));
		if(BLEN(b) < n)
			n = BLEN(b);
		memmove(a, b->rp, n);
		freeb(b);
		return n;
	case Qdata:
		b = readdata(sdp->conv[CONV(ch->qid)], n);
		if(b == nil)
			return 0;
		if(BLEN(b) < n)
			n = BLEN(b);
		memmove(a, b->rp, n);
		freeb(b);
		return n;
	case Qstats:
	case Qrstats:
		c = sdp->conv[CONV(ch->qid)];
		s = smalloc(1000);
		convstats(c, TYPE(ch->qid) == Qstats, s, 1000);
		rv = readstr(off, a, n, s);
		free(s);
		return rv;
	}
}

static Block*
sdpbread(Chan* ch, long n, ulong offset)
{
	Sdp *sdp = sdptab + ch->dev;

	if(TYPE(ch->qid) != Qdata)
		return devbread(ch, n, offset);
	return readdata(sdp->conv[CONV(ch->qid)], n);
}


static long
sdpwrite(Chan *ch, void *a, long n, vlong off)
{
	Sdp *sdp = sdptab + ch->dev;
	Cmdbuf *cb;
	char *arg0;
	char *p;
	Conv *c;
	Block *b;
	
	USED(off);
	switch(TYPE(ch->qid)) {
	default:
		error(Eperm);
	case Qctl:
		c = sdp->conv[CONV(ch->qid)];
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
		if(strcmp(arg0, "accept") == 0) {
			if(cb->nf != 2)
				error("usage: accect file");
			convopenchan(c, cb->f[1]);
		} else if(strcmp(arg0, "dial") == 0) {
			if(cb->nf != 2)
				error("usage: accect file");
			convopenchan(c, cb->f[1]);
			convsetstate(c, CDial);
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
	case Qcontrol:
		writecontrol(sdp->conv[CONV(ch->qid)], a, n, 0);
		return n;
	case Qdata:
		b = allocb(n);
		memmove(b->wp, a, n);
		b->wp += n;
		return writedata(sdp->conv[CONV(ch->qid)], b);
	}
}

long
sdpbwrite(Chan *ch, Block *bp, ulong offset)
{
	Sdp *sdp = sdptab + ch->dev;

	if(TYPE(ch->qid) != Qdata)
		return devbwrite(ch, bp, offset);
	return writedata(sdp->conv[CONV(ch->qid)], bp);
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
			memset(c, 0, sizeof(Conv));
			qlock(c);
			c->sdp = sdp;
			c->id = pp - sdp->conv;
			*pp = c;
			sdp->nconv++;
			break;
		}
		if(canqlock(c)){
			if(c->state == CClosed && c->reader == 0)
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
	c->in.window = ~0;
	c->in.compstate = malloc(sizeof(Unthwack));
	unthwackinit(c->in.compstate);
	c->out.compstate = malloc(sizeof(Thwack));
	thwackinit(c->out.compstate);
	strncpy(c->owner, up->user, sizeof(c->owner));
	c->perm = 0660;
	qunlock(c);

	return c;
}

// assume c is locked
static void
convretryinit(Conv *c)
{
	c->retries = 0;
	// +2 to avoid rounding effects.
	c->timeout = TK2SEC(m->ticks) + 2;
};

// assume c is locked
static int
convretry(Conv *c, int reset)
{
	c->retries++;
print("convretry: %s: %d\n", convstatename[c->state], c->retries);
	if(c->retries > MaxRetries) {
print("convretry: giving up\n");
		if(reset)
			convoput2(c, ConReset, c->dialid, c->acceptid);
		convsetstate(c, CClosed);
		return 0;
	}
	c->timeout = TK2SEC(m->ticks) + (c->retries+1);
	return 1;
}

static void
convtimer(Conv *c, ulong sec)
{
	Block *b;

	if(c->timeout > sec)
		return;
	qlock(c);
	if(waserror()) {
		qunlock(c);
		nexterror();
	}
	switch(c->state) {
	case CDial:
		if(convretry(c, 1))
			convoput2(c, ConOpenRequest, c->dialid, 0);
		break;
	case CAccept:
		if(convretry(c, 1))
			convoput2(c, ConOpenAck, c->dialid, c->acceptid);
		break;
	case COpen:
		b = c->out.controlpkt;
		if(b != nil) {
			if(convretry(c, 1))
				convoput(c, TControl, copyblock(b, blocklen(b)));
			break;
		}

		c->timeout = c->lastrecv + KeepAlive;
		if(c->timeout > sec)
			break;
		// keepalive - randomly spaced between KeepAlive and 2*KeepAlive
		if(c->timeout + KeepAlive > sec && nrand(c->lastrecv + 2*KeepAlive - sec) > 0)
			break;
print("sending keep alive: %ld\n", sec - c->lastrecv);
		// can not use writecontrol
		b = allocb(4);
		c->out.controlseq++;
		hnputl(b->wp, c->out.controlseq);
		b->wp += 4;
		c->out.controlpkt = b;
		convretryinit(c);
		if(!waserror()) {
			convoput(c, TControl, copyblock(b, blocklen(b)));
			poperror();
		}
		break;
	case CLocalClose:
		if(convretry(c, 0))
			convoput2(c, ConClose, c->dialid, c->acceptid);
		break;
	case CRemoteClose:
	case CClosed:
		c->timeout = ~0;
		break;
	}
	poperror();
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

print("convsetstate %s -> %s\n", convstatename[c->state], convstatename[state]);

	switch(state) {
	default:
		panic("setstate: bad state: %d", state);
	case CDial:
		assert(c->state == CInit);
		c->dialid = (rand()<<16) + rand();
		convretryinit(c);
		convoput2(c, ConOpenRequest, c->dialid, 0);
		break;
	case CAccept:
		assert(c->state == CInit);
		c->acceptid = (rand()<<16) + rand();
		convretryinit(c);
		convoput2(c, ConOpenAck, c->dialid, c->acceptid);
		break;
	case COpen:
		assert(c->state == CDial || c->state == CAccept);
		if(c->state == CDial) {
			convretryinit(c);
			convoput2(c, ConOpenAckAck, c->dialid, c->acceptid);
		}
		// setup initial key and auth method
		break;
	case CLocalClose:
		assert(c->state == CAccept || c->state == COpen);
		convretryinit(c);
		convoput2(c, ConClose, c->dialid, c->acceptid);
		break;
	case CRemoteClose:
		wakeup(&c->in.controlready);
		convoput2(c, ConReset, c->dialid, c->acceptid);
		break;
	case CClosed:
		wakeup(&c->in.controlready);
		wakeup(&c->out.controlready);
		if(c->readproc)
			postnote(c->readproc, 1, "interrupt", 0);
print("CClosed -> ref = %d\n", c->ref);
		if(c->chan) {	
			cclose(c->chan);
			c->chan = nil;
		}
		if(c->ref)
			break;
		if(c->channame) {
			free(c->channame);
			c->channame = nil;
		}
		if(c->ciphername) {
			free(c->ciphername);
			c->ciphername = nil;
		}
		if(c->authname) {
			free(c->authname);
			c->authname = nil;
		}
			if(c->compname) {
			free(c->compname);
			c->compname = nil;
		}
	strcpy(c->owner, "network");
		c->perm = 0660;
		c->dialid = 0;
		c->acceptid = 0;
		c->timeout = ~0;
		c->retries = 0;
		c->drop = 0;
		memset(c->masterkey, 0, sizeof(c->masterkey));
		onewaycleanup(&c->in);
		onewaycleanup(&c->out);
		memset(&c->lstats, 0, sizeof(Stats));
		memset(&c->rstats, 0, sizeof(Stats));
		break;
	}
	c->state = state;
}

static void
onewaycleanup(OneWay *ow)
{
	if(ow->controlpkt)
		freeb(ow->controlpkt);
	if(ow->authstate)
		free(ow->authstate);
	if(ow->cipherstate)
		free(ow->cipherstate);
	if(ow->compstate)
		free(ow->compstate);
	memset(ow, 0, sizeof(OneWay));
}


// assumes conv is locked
static void
convopenchan(Conv *c, char *path)
{
	if(c->state != CInit || c->chan != nil)
		error("already connected");
	c->chan = namec(path, Aopen, ORDWR, 0);
	c->channame = malloc(strlen(path)+1);
	strcpy(c->channame, path);
	if(waserror()) {
		cclose(c->chan);
		c->chan = nil;
		free(c->channame);
		c->channame = nil;
		nexterror();
	}
	kproc("convreader", convreader, c);
	c->reader = 1;
	poperror();
}

static void
convstats(Conv *c, int local, char *buf, int n)
{
	Stats *stats;
	char *p, *ep;

	if(local) {
		stats = &c->lstats;
	} else {
		if(!waserror()) {
			writecontrol(c, 0, 0, 1);
			poperror();
		}
		stats = &c->rstats;
	}

	qlock(c);
	p = buf;
	ep = buf + n;
	p += snprint(p, ep-p, "outPackets: %ld\n", stats->outPackets);
	p += snprint(p, ep-p, "outDataPackets: %ld\n", stats->outDataPackets);
	p += snprint(p, ep-p, "outDataBytes: %ld\n", stats->outDataBytes);
	p += snprint(p, ep-p, "outCompDataBytes: %ld\n", stats->outCompDataBytes);
	p += snprint(p, ep-p, "inPackets: %ld\n", stats->inPackets);
	p += snprint(p, ep-p, "inDataPackets: %ld\n", stats->inDataPackets);
	p += snprint(p, ep-p, "inDataBytes: %ld\n", stats->inDataBytes);
	p += snprint(p, ep-p, "inCompDataBytes: %ld\n", stats->inCompDataBytes);
	p += snprint(p, ep-p, "inMissing: %ld\n", stats->inMissing);
	p += snprint(p, ep-p, "inDup: %ld\n", stats->inDup);
	p += snprint(p, ep-p, "inReorder: %ld\n", stats->inReorder);
	p += snprint(p, ep-p, "inBadAuth: %ld\n", stats->inBadAuth);
	p += snprint(p, ep-p, "inBadSeq: %ld\n", stats->inBadSeq);
	USED(p);
	qunlock(c);
}

// c is locked
static void
convack(Conv *c)
{
	Block *b;
	AckPkt *ack;
	Stats *s;

	b = allocb(sizeof(AckPkt));
	ack = (AckPkt*)b->wp;
	b->wp += sizeof(AckPkt);
	s = &c->lstats;
	hnputl(ack->cseq, c->in.controlseq);
	hnputl(ack->outPackets, s->outPackets);
	hnputl(ack->outDataPackets, s->outDataPackets);
	hnputl(ack->outDataBytes, s->outDataBytes);
	hnputl(ack->outCompDataBytes, s->outCompDataBytes);
	hnputl(ack->inPackets, s->inPackets);
	hnputl(ack->inDataPackets, s->inDataPackets);
	hnputl(ack->inDataBytes, s->inDataBytes);
	hnputl(ack->inCompDataBytes, s->inCompDataBytes);
	hnputl(ack->inMissing, s->inMissing);
	hnputl(ack->inDup, s->inDup);
	hnputl(ack->inReorder, s->inReorder);
	hnputl(ack->inBadAuth, s->inBadAuth);
	hnputl(ack->inBadSeq, s->inBadSeq);
	convoput(c, TControlAck, b);
}


// assume we hold lock for c
static Block *
conviput(Conv *c, Block *b, int control)
{
	int type, n;
	ulong seq, seqwrap, cseq;
	long seqdiff;
	AckPkt *ack;
	ulong mseq, mask;
	Block *bb;

	c->lstats.inPackets++;

	if(BLEN(b) < 4) {
		freeb(b);
		return nil;
	}
	
	type = b->rp[0];
	if(type == TConnect) {
		conviput2(c, b);
		return nil;
	}

	seq = (b->rp[1]<<16) + (b->rp[2]<<8) + b->rp[3];
	b->rp += 4;

	seqwrap = c->in.seqwrap;
	seqdiff = seq - c->in.seq;
	if(seqdiff < -(SeqMax*3/4)) {
		seqwrap++;
		seqdiff += SeqMax;
	} else if(seqdiff > SeqMax*3/4) {
		seqwrap--;
		seqdiff -= SeqMax;
	}

	if(seqdiff <= 0) {
		if(seqdiff <= -SeqWindow) {
print("old sequence number: %ld (%ld %ld)\n", seq, c->in.seqwrap, seqdiff);
			c->lstats.inBadSeq++;
			freeb(b);
			return nil;
		}

		if(c->in.window & (1<<-seqdiff)) {
print("dup sequence number: %ld (%ld %ld)\n", seq, c->in.seqwrap, seqdiff);
			c->lstats.inDup++;
			freeb(b);
			return nil;
		}

		c->lstats.inReorder++;
	}

	// ok the sequence number looks ok
if(0) print("coniput seq=%ulx\n", seq);
	// auth
	// decrypt

	// ok the packet is good
	if(seqdiff > 0) {
		while(seqdiff > 0 && c->in.window != 0) {
			if((c->in.window & (1<<(SeqWindow-1))) == 0) {
print("missing packet: %ld\n", seq - seqdiff);
				c->lstats.inMissing++;
			}
			c->in.window <<= 1;
			seqdiff--;
		}
		if(seqdiff > 0) {
print("missing packets: %ld-%ld\n", seq - SeqWindow - seqdiff+1, seq-SeqWindow);
			c->lstats.inMissing += seqdiff;
		}
		c->in.seq = seq;
		c->in.seqwrap = seqwrap;
		c->in.window |= 1;
	}
	c->lastrecv = TK2SEC(m->ticks);

	switch(type) {
	case TControl:
		if(BLEN(b) < 4)
			break;
		cseq = nhgetl(b->rp);
		if(cseq == c->in.controlseq) {
print("duplicate control packet: %ulx\n", cseq);
			// duplicate control packet
			freeb(b);
			if(c->in.controlpkt == nil)
				convack(c);
			return nil;
		}

		if(cseq != c->in.controlseq+1)
			break;
		c->in.controlseq = cseq;
		b->rp += 4;
		if(BLEN(b) == 0) {
			// just a ping
			freeb(b);
			convack(c);
		} else {
			c->in.controlpkt = b;
if(0) print("recv %ld size=%ld\n", cseq, BLEN(b));
			wakeup(&c->in.controlready);
		}
		return nil;
	case TControlAck:
		if(BLEN(b) != sizeof(AckPkt))
			break;
		ack = (AckPkt*)(b->rp);
		cseq = nhgetl(ack->cseq);
		if(cseq != c->out.controlseq) {
print("ControlAck expected %ulx got %ulx\n", c->out.controlseq, cseq);
			break;
		}
		c->rstats.outPackets = nhgetl(ack->outPackets);
		c->rstats.outDataPackets = nhgetl(ack->outDataPackets);
		c->rstats.outDataBytes = nhgetl(ack->outDataBytes);
		c->rstats.outCompDataBytes = nhgetl(ack->outCompDataBytes);
		c->rstats.inPackets = nhgetl(ack->inPackets);
		c->rstats.inDataPackets = nhgetl(ack->inDataPackets);
		c->rstats.inDataBytes = nhgetl(ack->inDataBytes);
		c->rstats.inCompDataBytes = nhgetl(ack->inCompDataBytes);
		c->rstats.inMissing = nhgetl(ack->inMissing);
		c->rstats.inDup = nhgetl(ack->inDup);
		c->rstats.inReorder = nhgetl(ack->inReorder);
		c->rstats.inBadAuth = nhgetl(ack->inBadAuth);
		c->rstats.inBadSeq = nhgetl(ack->inBadSeq);
		freeb(b);
		freeb(c->out.controlpkt);
		c->out.controlpkt = nil;
		c->timeout = c->lastrecv + KeepAlive;
		wakeup(&c->out.controlready);
		return nil;
	case TData:
		c->lstats.inDataPackets++;
		c->lstats.inDataBytes += BLEN(b);
		c->lstats.inCompDataBytes += BLEN(b);
		if(control)
			break;
		return b;
	case TThwackU:
		c->lstats.inDataPackets++;
		c->lstats.inCompDataBytes += BLEN(b);
		mask = b->rp[0];
		mseq = (b->rp[1]<<16) | (b->rp[2]<<8) | b->rp[3];
		b->rp += 4;
		thwackack(c->out.compstate, mseq, mask);
		c->lstats.inDataBytes += BLEN(b);
		if(control)
			break;
		return b;
	case TThwackC:
		c->lstats.inDataPackets++;
		c->lstats.inCompDataBytes += BLEN(b);
		bb = b;
		b = allocb(ThwMaxBlock);
		n = unthwack(c->in.compstate, b->wp, ThwMaxBlock, bb->rp, BLEN(bb), seq);
		freeb(bb);
		if(n < 0)
			break;
		b->wp += n;
		mask = b->rp[0];
		mseq = (b->rp[1]<<16) | (b->rp[2]<<8) | b->rp[3];
		thwackack(c->out.compstate, mseq, mask);
		b->rp += 4;
		c->lstats.inDataBytes += BLEN(b);
		if(control)
			break;
		return b;
	}
print("droping packet %d n=%ld\n", type, BLEN(b));
	freeb(b);
	return nil;
}

// assume hold conv lock
static void
conviput2(Conv *c, Block *b)
{
	ConnectPkt *con;
	ulong dialid;
	ulong acceptid;

	if(BLEN(b) != sizeof(ConnectPkt)) {
		freeb(b);
		return;
	}
	con = (ConnectPkt*)b->rp;
	dialid = nhgetl(con->dialid);
	acceptid = nhgetl(con->acceptid);

print("conviput2: %s: %d %uld %uld\n", convstatename[c->state], con->op, dialid, acceptid);

	switch(c->state) {
	default:
		panic("unknown state: %d", c->state);
	case CInit:
		break;
	case CDial:
		if(dialid != c->dialid)
			goto Reset;
		break;
	case CAccept:
	case COpen:
	case CLocalClose:
	case CRemoteClose:
		if(dialid != c->dialid || acceptid != c->acceptid)
			goto Reset;
		break;
	case CClosed:
		goto Reset;
	}

	switch(con->op) {
	case ConOpenRequest:
		switch(c->state) {
		case CInit:
			c->dialid = dialid;
			convsetstate(c, CAccept);
			return;
		case CAccept:
		case COpen:
			// duplicate ConOpenRequest that we ignore
			return;
		}
		break;
	case ConOpenAck:
		switch(c->state) {
		case CDial:
			c->acceptid = acceptid;
			convsetstate(c, COpen);
			return;
		case COpen:
			// duplicate that we have to ack
			convoput2(c, ConOpenAckAck, acceptid, dialid);
			return;
		}
		break;
	case ConOpenAckAck:
		switch(c->state) {
		case CAccept:
			convsetstate(c, COpen);
			return;
		case COpen:
			// duplicate that we ignore
			return;
		}
		break;
	case ConClose:
		convoput2(c, ConReset, dialid, acceptid);
		switch(c->state) {
		case CInit:
		case CDial:
		case CAccept:
		case CLocalClose:
			convsetstate(c, CClosed);
			return;
		case COpen:
			convsetstate(c, CRemoteClose);
			return;
		case CRemoteClose:
			return;
		}
		return;
	case ConReset:
		switch(c->state) {
		case CInit:
		case CDial:
		case CAccept:
		case COpen:
		case CLocalClose:
			convsetstate(c, CClosed);
			return;
		case CRemoteClose:
			return;
		}
		return;
	}
Reset:
	// invalid connection message - reset to sender
print("invalid conviput2 - sending reset\n");
	convoput2(c, ConReset, dialid, acceptid);
}

// c is locked
static void
convwriteblock(Conv *c, Block *b)
{
	// simulated errors
	if(c->drop && nrand(c->drop) == 0)
		return;

	if(waserror()) {
		convsetstate(c, CClosed);
		nexterror();
	}
	devtab[c->chan->type]->bwrite(c->chan, b, 0);
	poperror();
}


// assume hold conv lock
static void
convoput(Conv *c, int type, Block *b)
{
	// try and compress
	c->lstats.outPackets++;
	/* Make space to fit sdp header */
	b = padblock(b, 4 + c->out.cipherivlen);
	b->rp[0] = type;
	c->out.seq++;
	if(c->out.seq == (1<<24)) {
		c->out.seq = 0;
		c->out.seqwrap++;
	}
	b->rp[1] = c->out.seq>>16;
	b->rp[2] = c->out.seq>>8;
	b->rp[3] = c->out.seq;
	
	// encrypt
	// auth
	
	convwriteblock(c, b);
}

// assume hold conv lock
static void
convoput2(Conv *c, int op, ulong dialid, ulong acceptid)
{
	Block *b;
	ConnectPkt *con;

	c->lstats.outPackets++;
	if(c->chan == nil) {
print("chan = nil\n");
		error("no channel attached");
	}
	b = allocb(sizeof(ConnectPkt));
	con = (ConnectPkt*)b->wp;
	b->wp += sizeof(ConnectPkt);
	con->type = TConnect;
	con->op = op;
	hnputl(con->dialid, dialid);
	hnputl(con->acceptid, acceptid);

	convwriteblock(c, b);
}

static Block *
convreadblock(Conv *c, int n)
{
	Block *b;
	Chan *ch = nil;

	qlock(&c->readlk);
	if(waserror()) {
		c->readproc = nil;
		if(ch)
			cclose(ch);
		qunlock(&c->readlk);
		nexterror();
	}
	qlock(c);
	if(c->state == CClosed) {
		qunlock(c);
		error("closed");
	}
	c->readproc = up;
	ch = c->chan;
	incref(ch);
	qunlock(c);

	b = devtab[ch->type]->bread(ch, n, 0);
	c->readproc = nil;
	cclose(ch);
	poperror();
	qunlock(&c->readlk);

	return b;
}

static int
readready(void *a)
{
	Conv *c = a;

	return c->in.controlpkt != nil || (c->state == CClosed) || (c->state == CRemoteClose);
}

static Block *
readcontrol(Conv *c, int n)
{
	Block *b;

	USED(n);

	qlock(&c->in.controllk);
	if(waserror()) {
		qunlock(&c->in.controllk);
		nexterror();
	}
	qlock(c);	// this lock is not held during the sleep below

	for(;;) {
		if(c->state == CInit || c->state == CClosed) {
			qunlock(c);
print("readcontrol: return error - state = %s\n", convstatename[c->state]);
			error("conversation closed");
		}

		if(c->in.controlpkt != nil)
			break;

		if(c->state == CRemoteClose) {
			qunlock(c);
print("readcontrol: return nil - state = %s\n", convstatename[c->state]);
			poperror();
			return nil;
		}
		qunlock(c);
		sleep(&c->in.controlready, readready, c);
		qlock(c);
	}

	convack(c);

	b = c->in.controlpkt;
	c->in.controlpkt = nil;
	qunlock(c);
	poperror();
	qunlock(&c->in.controllk);
	return b;
}


static int
writeready(void *a)
{
	Conv *c = a;

	return c->out.controlpkt == nil || (c->state == CClosed) || (c->state == CRemoteClose);
}

// c is locked
static void
writewait(Conv *c)
{
	for(;;) {
		if(c->state == CInit || c->state == CClosed || c->state == CRemoteClose) {
print("writecontrol: return error - state = %s\n", convstatename[c->state]);
			error("conversation closed");
		}

		if(c->state == COpen && c->out.controlpkt == nil)
			break;

		qunlock(c);
		if(waserror()) {
			qlock(c);
			nexterror();
		}
		sleep(&c->out.controlready, writeready, c);
		poperror();
		qlock(c);
	}
}

static void
writecontrol(Conv *c, void *p, int n, int wait)
{
	Block *b;


	qlock(&c->out.controllk);
	qlock(c);
	if(waserror()) {
		qunlock(c);
		qunlock(&c->out.controllk);
		nexterror();
	}
	writewait(c);
	b = allocb(4+n);
	c->out.controlseq++;
	hnputl(b->wp, c->out.controlseq);
	memmove(b->wp+4, p, n);
	b->wp += 4+n;
	c->out.controlpkt = b;
	convretryinit(c);
	convoput(c, TControl, copyblock(b, blocklen(b)));
	if(wait)
		writewait(c);
	poperror();
	qunlock(c);
	qunlock(&c->out.controllk);
}

static Block *
readdata(Conv *c, int n)
{
	Block *b;

	for(;;) {
		b = convreadblock(c, n);
		if(b == nil)
			return nil;
		qlock(c);
		if(waserror()) {
			qunlock(c);
			return nil;
		}
		b = conviput(c, b, 0);
		poperror();
		qunlock(c);
		if(b != nil)
			return b;
	}
}

static long
writedata(Conv *c, Block *b)
{
	int n, nn;
	ulong seq;
	Block *bb;

	qlock(c);
	if(waserror()) {
		qunlock(c);
		nexterror();
	}

	if(c->state != COpen) {
		freeb(b);
		error("conversation not open");
	}

	n = BLEN(b);
	c->lstats.outDataPackets++;
	c->lstats.outDataBytes += n;

	if(0) {
		c->lstats.outCompDataBytes += n;
		convoput(c, TData, b);
		poperror();
		qunlock(c);
		return n;
	}
	b = padblock(b, 4);
	b->rp[0] = (c->in.window>>1) & 0xff;
	b->rp[1] = c->in.seq>>16;
	b->rp[2] = c->in.seq>>8;
	b->rp[3] = c->in.seq;

	// must generate same value as convoput
	seq = (c->out.seq + 1) & (SeqMax-1);

	bb = allocb(BLEN(b));
	nn = thwack(c->out.compstate, bb->wp, b->rp, BLEN(b), seq);
	if(nn < 0) {
		c->lstats.outCompDataBytes += BLEN(b);
		convoput(c, TThwackU, b);
		freeb(bb);
	} else {
		c->lstats.outCompDataBytes += nn;
		bb->wp += nn;
		convoput(c, TThwackC, bb);
		freeb(b);
	}

	poperror();
	qunlock(c);
	return n;
}

static void
convreader(void *a)
{
	Conv *c = a;
	Block *b;

print("convreader\n");
	qlock(c);
	assert(c->reader == 1);
	while(c->dataopen == 0 && c->state != CClosed) {
		qunlock(c);
		b = nil;
		if(!waserror()) {
			b = convreadblock(c, 2000);
			poperror();
		}
		qlock(c);
		if(b == nil) {
print("up->error = %s\n", up->error);
			if(strcmp(up->error, Eintr) != 0) {
				if(!waserror()) {
					convsetstate(c, CClosed);
					poperror();
				}
				break;
			}
		} else if(!waserror()) {
			conviput(c, b, 1);
			poperror();
		}
	}
print("convreader exiting\n");
	c->reader = 0;
	qunlock(c);
	pexit("hangup", 1);
}
