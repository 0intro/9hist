#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/netif.h"
#include "../port/error.h"

#include	<libcrypt.h>

/*
 * sdp - secure datagram protocol
 */

typedef struct Sdp Sdp;
typedef struct Conv Conv;
typedef struct OneWay OneWay;
typedef struct ConnectPkt ConnectPkt;

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

	Maxconv= 256,		// power of 2
	Nfs= 4,				// number of file systems
	MaxRetries=	8,
	KeepAlive = 60,		// keep alive in seconds
	KeyLength= 32,
};

#define TYPE(x) 	((x).path & 0xff)
#define CONV(x) 	(((x).path >> 8)&(Maxconv-1))
#define QID(x, y) 	(((x)<<8) | (y))

struct OneWay
{
	ulong	seqwrap;	// number of wraps of the sequence number
	ulong	seq;
	ulong	window;

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


	ulong	timeout;
	int		retries;

	// the following pair uniquely define conversation on this port
	ulong dialid;
	ulong acceptid;

	Proc *readproc;
	QLock readlk;
	Chan *chan;	// packet channel

	char owner[NAMELEN];		/* protections */
	int	perm;

	uchar	masterkey[KeyLength];

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
	ConClose,
	ConCloseAck,
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
static char *convstatename[] = {
	[CInit] "Init",
	[CDial] "Dial",
	[CAccept] "Accept",
	[COpen] "Open",
	[CClosing] "Closing",
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
static Block *readcontrol(Conv *c, int n);
static Block *readdata(Conv *c, int n);
static void convoput(Conv *c, int type, Block *b);
static void convoput2(Conv *c, int op, ulong dialid, ulong acceptid);


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
		if(strcmp(up->user, c->owner) != 0 || (perm & c->perm) != perm)
				error(Eperm);
		c->ref++;
		if(TYPE(ch->qid) == Qdata)
			c->dataopen++;
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
			if(c->dataopen == 0)
				wakeup(&c->in.controlready);
		}
print("close c->ref = %d\n", c->ref);
		if(c->ref == 0) {
			switch(c->state) {
			default:
				convsetstate(c, CClosed);
				break;
			case CAccept:
			case COpen:
				convsetstate(c, CClosing);
				break;
			case CClosing:
				break;
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
	Sdp *sdp = sdptab + ch->dev;
	Conv *c;
	Block *b;

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
			convsetstate(c, CAccept);
		} else if(strcmp(arg0, "dial") == 0) {
			if(cb->nf != 1)
				error("usage: dial");
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
convretry(Conv *c)
{
	c->retries++;
	if(c->retries > MaxRetries) {
print("convretry: giving up\n");
		convsetstate(c, CClosed);
		return 0;
	}
	c->timeout = TK2SEC(m->ticks) + (c->retries+1);
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
print("convtimer: %s\n", convstatename[c->state]);
	switch(c->state) {
	case CDial:
		if(convretry(c))
			convoput2(c, ConOpenRequest, c->dialid, 0);
		break;
	case CAccept:
		if(convretry(c))
			convoput2(c, ConOpenAck, c->dialid, c->acceptid);
		break;
	case COpen:
		// check for control packet and keepalive
		break;
	case CClosing:
		if(convretry(c))
			convoput2(c, ConClose, c->dialid, c->acceptid);
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
			convoput2(c, ConOpenAck, c->dialid, c->acceptid);
		}
		// setup initial key and auth method
		break;
	case CClosing:
		assert(c->state == COpen);
		convretryinit(c);
		convoput2(c, ConClose, c->dialid, c->acceptid);
		break;
	case CClosed:
		if(c->readproc)
			postnote(c->readproc, 1, "interrupt", 0);
		if(c->ref)
			break;
		if(c->chan) {	
			cclose(c->chan);
			c->chan = nil;
		}
		strcpy(c->owner, "network");
		c->perm = 0660;
		c->dialid = 0;
		c->acceptid = 0;
		c->timeout = 0;
		c->retries = 0;
		c->drop = 0;
		memset(c->masterkey, 0, sizeof(c->masterkey));
		onewaycleanup(&c->in);
		onewaycleanup(&c->out);
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


static Block *
convreadblock(Conv *c, int n)
{
	Block *b;

	qlock(&c->readlk);
	if(waserror()) {
		c->readproc = nil;
		qunlock(&c->readlk);
		nexterror();
	}
	qlock(c);
	if(c->state == CClosed) {
		qunlock(c);
		poperror();
		qunlock(&c->readlk);
		return 0;
	}
	c->readproc = up;
	qunlock(c);

	b = devtab[c->chan->type]->bread(c->chan, n, 0);
	c->readproc = nil;
	poperror();
	qunlock(&c->readlk);

	return b;
}


// assume we hold lock for c
static Block *
conviput(Conv *c, Block *b, int control)
{
	int type;
	ulong seq, cseq;

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

	USED(seq);
	// auth
	// decrypt

	// ok the packet is good

	switch(type) {
	case TControl:
		if(BLEN(b) <= 4)
			break;
		cseq = nhgetl(b->rp);
		if(cseq == c->in.controlseq) {
			// duplicate control packet
			// send ack
			b->wp = b->rp + 4;
			convoput(c, TControlAck, b);
			return nil;
		}

		if(cseq != c->in.controlseq+1)
			break;
	
		c->in.controlseq = cseq;
		b->rp += 4;
		if(control)
			return b;
		c->in.controlpkt = b;
		wakeup(&c->in.controlready);
		return nil;
	case TControlAck:
		if(BLEN(b) != 4)
			break;
		cseq = nhgetl(b->rp);
		if(cseq != c->out.controlseq)
			break;
		freeb(b);
		freeb(c->out.controlpkt);
		c->out.controlpkt = 0;
		wakeup(&c->out.controlready);
		return nil;
	case TData:
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

print("conviput2: %d %uld %uld\n", con->op, dialid, acceptid);
	switch(con->op) {
	case ConOpenRequest:
		switch(c->state) {
		default:
			convoput2(c, ConReset, dialid, acceptid);
			break;
		case CInit:
			c->dialid = dialid;
			convsetstate(c, CAccept);
			break;
		case CAccept:
		case COpen:
			if(dialid != c->dialid || acceptid != c->acceptid)
				convoput2(c, ConReset, dialid, acceptid);
			break;
		}
		break;
	case ConOpenAck:
		switch(c->state) {
		case CDial:
			if(dialid != c->dialid) {
				convoput2(c, ConReset, dialid, acceptid);
				break;
			}
			c->acceptid = acceptid;
			convsetstate(c, COpen);
			break;
		case CAccept:
			if(dialid != c->dialid || acceptid != c->acceptid) {
				convoput2(c, ConReset, dialid, acceptid);
				break;
			}
			convsetstate(c, COpen);
		}
		break;
	case ConClose:
		convoput2(c, ConCloseAck, dialid, acceptid);
		// fall though
	case ConReset:
		switch(c->state) {
		case CDial:
			if(dialid == c->dialid)
				convsetstate(c, CClosed);
			break;
		case CAccept:
		case COpen:
		case CClosing:
			if(dialid == c->dialid && acceptid == c->acceptid)
				convsetstate(c, CClosed);
			break;
		}
	case ConCloseAck:
		if(c->state == CClosing && dialid == c->dialid && acceptid == c->acceptid)
			convsetstate(c, CClosed);
		break;
	}
}

// assume hold conv lock
static void
convoput(Conv *c, int type, Block *b)
{
	// try and compress

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

	// simulated errors
	if(c->drop && c->drop > nrand(c->drop))
		return;
	devtab[c->chan->type]->bwrite(c->chan, b, 0);
}

// assume hold conv lock
static void
convoput2(Conv *c, int op, ulong dialid, ulong acceptid)
{
	ConnectPkt con;

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

static int
readready(void *a)
{
	Conv *c = a;

	return (c->state == CClosed) || c->in.controlpkt != nil || c->dataopen == 0;
}

static Block *
readcontrol(Conv *c, int n)
{
	Block *b;

	for(;;) {
		qlock(c);
		if(c->state == CClosed || c->state == CInit) {
			qunlock(c);
			return nil;
		}

		if(c->in.controlpkt != nil) {
			b = c->in.controlpkt;
			c->in.controlpkt = nil;
			qunlock(c);
			return b;
		}
		qunlock(c);

		// hack - this is to avoid gating onto the
		// read which will in general result in excessive
		// context switches.
		// The assumed behavior is that the client will read
		// from the control channel until the session is authenticated
		// at which point it will open the data channel and
		// start reading on that.  After the data channel is opened,
		// read on the channel are required for packets to
		// be delivered to the control channel

		if(c->dataopen) {
			sleep(&c->in.controlready, readready, c);
		} else {
			b = convreadblock(c, n);
			if(b == nil)
				return nil;
			qlock(c);
			if(waserror()) {
				qunlock(c);
				return nil;
			}
			b = conviput(c, b, 1);
			poperror();
			qunlock(c);
			if(b != nil)
				return b;
		}
	}
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
