/*
 *  devtls - record layer for transport layer security 1.0 and secure sockets layer 3.0
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	<libsec.h>

typedef struct OneWay OneWay;
typedef struct Secret Secret;

enum {
	MaxRecLen	= 1<<14,	/* max payload length of a record layer message */
	RecHdrLen	= 5,
	TLSVersion	= 0x0301,
	SSL3Version	= 0x0300,
	ProtocolVersion	= 0x0301,	/* maximum version we speak */
	MinProtoVersion	= 0x0300,	/* limits on version we accept */
	MaxProtoVersion	= 0x03ff,
};

/* connection states */
enum {
	SHandshake,		// doing handshake
	SOpen,			// application data can be sent
	SRemoteClosed,		// remote side has closed down
	SError,			// some sort of error has occured
	SClosed,		// it is all over
};

/* record types */
enum {
	RChangeCipherSpec = 20,
	RAlert,
	RHandshake,
	RApplication,
};

/* alerts */
enum {
	ECloseNotify = 0,
	EUnexpectedMessage = 10,
	EBadRecordMac = 20,
	EDecryptionFailed = 21,
	ERecordOverflow = 22,
	EDecompressionFailure = 30,
	EHandshakeFailure = 40,
	ENoCertificate = 41,
	EBadCertificate = 42,
	EUnsupportedCertificate = 43,
	ECertificateRevoked = 44,
	ECertificateExpired = 45,
	ECertificateUnknown = 46,
	EIllegalParameter = 47,
	EUnknownCa = 48,
	EAccessDenied = 49,
	EDecodeError = 50,
	EDecryptError = 51,
	EExportRestriction = 60,
	EProtocolVersion = 70,
	EInsufficientSecurity = 71,
	EInternalError = 80,
	EUserCanceled = 90,
	ENoRenegotiation = 100,

	EMAX = 256
};

struct Secret
{
	DigestState	*(*mac)(uchar*, ulong, uchar*, ulong, uchar*, DigestState*);
	int		maclen;
	RC4state	rc4;
	uchar		mackey[64];
};

struct OneWay
{
	QLock		io;		/* locks io access */
	QLock		seclock;	/* locks secret paramaters */
	ulong		seq;
	Secret		*sec;		/* cipher in use */
	Secret		*new;		/* cipher waiting for enable */
};

typedef struct TlsRec TlsRec;
struct TlsRec
{
	Chan		*c;			/* io channel */
	int		ref;			/* serialized by dslock for atomic destroy */
	int		version;		/* version of the protocol we are speaking */
	int		verset;			/* version has been set */

	Lock		statelk;
	int		state;			/* must be set using setstate */

	/* record layer mac functions for different protocol versions */
	void		(*packMac)(Secret*, uchar*, uchar*, uchar*, uchar*, int, uchar*);

	/* input side -- protected by in.q */
	OneWay	in;
	Queue	*handq;				/* queue of handshake messages */
	Block	*processed;			/* next bunch of application data */
	Block	*unprocessed;			/* data read from c but not parsed into records */

	/* output side */
	OneWay	out;

	/* protections */
	char	user[NAMELEN];
	int	perm;
};

typedef struct TlsErrs	TlsErrs;
struct TlsErrs{
	int	err;
	int	sslerr;
	int	tlserr;
	int	fatal;
	char	*msg;
};

static TlsErrs tlserrs[] = {
	{ECloseNotify,			ECloseNotify,			ECloseNotify,
		0, "remote close"},
	{EUnexpectedMessage,		EUnexpectedMessage,		EUnexpectedMessage,
		1, "unexpected message"},
	{EBadRecordMac,			EBadRecordMac,			EBadRecordMac,
		1, "bad record MAC"},
	{EDecryptionFailed,		EIllegalParameter,		EDecryptionFailed,
		1, "decryption failed"},
	{ERecordOverflow,		EIllegalParameter,		ERecordOverflow,
		1, "record too long"},
	{EDecompressionFailure,		EDecompressionFailure,		EDecompressionFailure,
		1, "decompression failed"},
	{EHandshakeFailure,		EHandshakeFailure,		EHandshakeFailure,
		1, "could not negotiate acceptable security paramters"},
	{ENoCertificate,		ENoCertificate,			ECertificateUnknown,
		1, "no appropriate certificate available"},
	{EBadCertificate,		EBadCertificate,		EBadCertificate,
		1, "corrupted or invalid certificate"},
	{EUnsupportedCertificate,	EUnsupportedCertificate,	EUnsupportedCertificate,
		1, "unsupported certificate type"},
	{ECertificateRevoked,		ECertificateRevoked,		ECertificateRevoked,
		1, "revoked certificate"},
	{ECertificateExpired,		ECertificateExpired,		ECertificateExpired,
		1, "expired certificate"},
	{ECertificateUnknown,		ECertificateUnknown,		ECertificateUnknown,
		1, "unacceptable certificate"},
	{EIllegalParameter,		EIllegalParameter,		EIllegalParameter,
		1, "illegal parameter"},
	{EUnknownCa,			EHandshakeFailure,		EUnknownCa,
		1, "unknown certificate authority"},
	{EAccessDenied,			EHandshakeFailure,		EAccessDenied,
		1, "access denied"},
	{EDecodeError,			EIllegalParameter,		EDecodeError,
		1, "error decoding message"},
	{EDecryptError,			EIllegalParameter,		EDecryptError,
		1, "error decrypting message"},
	{EExportRestriction,		EHandshakeFailure,		EExportRestriction,
		1, "export restriction violated"},
	{EProtocolVersion,		EIllegalParameter,		EProtocolVersion,
		1, "protocol version not supported"},
	{EInsufficientSecurity,		EHandshakeFailure,		EInsufficientSecurity,
		1, "stronger security routines required"},
	{EInternalError,		EHandshakeFailure,		EInternalError,
		1, "internal error"},
	{EUserCanceled,			ECloseNotify,		EUserCanceled,
		0, "handshake canceled by user"},
	{ENoRenegotiation,		EUnexpectedMessage,		ENoRenegotiation,
		0, "renegotiation not supported"},
	{-1},
};

static	Lock	dslock;
static	int	dshiwat;
static	int	maxdstate = 128;
static	TlsRec** dstate;
static	char	*encalgs;
static	char	*hashalgs;

enum
{
//ZZZ
	Maxdmsg=	1<<16,
	Maxdstate=	64
};

enum{
	Qtopdir		= 1,	/* top level directory */
	Qprotodir,
	Qclonus,
	Qconvdir,		/* directory for a conversation */
	Qdata,
	Qctl,
	Qhand,
	Qencalgs,
	Qhashalgs,
};

#define TYPE(x) 	((x).path & 0xf)
#define CONV(x) 	(((x).path >> 5)&(Maxdstate-1))
#define QID(c, y) 	(((c)<<5) | (y))

static void	ensure(TlsRec*, Block**, int);
static void	consume(Block**, uchar*, int);
static Chan*	buftochan(char*);
static void	tlshangup(TlsRec*);
static TlsRec*	dsclone(Chan *c);
static void	dsnew(Chan *c, TlsRec **);
static DigestState*sslmac_md5(uchar *p, ulong len, uchar *key, ulong klen, uchar *digest, DigestState *s);
static void	sslPackMac(Secret *sec, uchar *mackey, uchar *seq, uchar *header, uchar *body, int len, uchar *mac);
static void	tlsPackMac(Secret *sec, uchar *mackey, uchar *seq, uchar *header, uchar *body, int len, uchar *mac);
static void	put64(uchar *p, vlong x);
static void	put32(uchar *p, u32int);
static void	put24(uchar *p, int);
static void	put16(uchar *p, int);
static u32int	get32(uchar *p);
static int	get16(uchar *p);
static void	tlsSetState(TlsRec *tr, int newstate);
static void	rcvAlert(TlsRec *tr, int err);
static void	sendAlert(TlsRec *tr, int err);
static void	rcvError(TlsRec *tr, int err, char *msg, ...);
#pragma	varargck	argpos	rcvError	3

static char *tlsnames[] = {
[Qclonus]	"clone",
[Qdata]		"data",
[Qctl]		"ctl",
[Qhand]		"hand",
[Qencalgs]	"encalgs",
[Qhashalgs]	"hashalgs",
};

static int
tlsgen(Chan *c, Dirtab *d, int nd, int s, Dir *dp)
{
	Qid q;
	TlsRec *ds;
	char name[16], *p, *nm;

	USED(nd);
	USED(d);
	q.vers = 0;
	switch(TYPE(c->qid)) {
	case Qtopdir:
		if(s == DEVDOTDOT){
			q.path = QID(0, Qtopdir)|CHDIR;
			devdir(c, q, "#D", 0, eve, CHDIR|0555, dp);
			return 1;
		}
		if(s > 0)
			return -1;
		q.path = QID(0, Qprotodir)|CHDIR;
		devdir(c, q, "tls", 0, eve, CHDIR|0555, dp);
		return 1;
	case Qprotodir:
		if(s == DEVDOTDOT){
			q.path = QID(0, Qtopdir)|CHDIR;
			devdir(c, q, ".", 0, eve, CHDIR|0555, dp);
			return 1;
		}
		if(s < dshiwat) {
			sprint(name, "%d", s);
			q.path = QID(s, Qconvdir)|CHDIR;
			ds = dstate[s];
			if(ds != 0)
				nm = ds->user;
			else
				nm = eve;
			devdir(c, q, name, 0, nm, CHDIR|0555, dp);
			return 1;
		}
		if(s > dshiwat)
			return -1;
		q.path = QID(0, Qclonus);
		devdir(c, q, "clone", 0, eve, 0555, dp);
		return 1;
	case Qconvdir:
		if(s == DEVDOTDOT){
			q.path = QID(0, Qprotodir)|CHDIR;
			devdir(c, q, "tls", 0, eve, CHDIR|0555, dp);
			return 1;
		}
		ds = dstate[CONV(c->qid)];
		if(ds != 0)
			nm = ds->user;
		else
			nm = eve;
		switch(s) {
		default:
			return -1;
		case 0:
			q.path = QID(CONV(c->qid), Qctl);
			p = "ctl";
			break;
		case 1:
			q.path = QID(CONV(c->qid), Qdata);
			p = "data";
			break;
		case 2:
			q.path = QID(CONV(c->qid), Qhand);
			p = "hand";
			break;
		case 3:
			q.path = QID(CONV(c->qid), Qencalgs);
			p = "encalgs";
			break;
		case 4:
			q.path = QID(CONV(c->qid), Qhashalgs);
			p = "hashalgs";
			break;
		}
		devdir(c, q, p, 0, nm, 0660, dp);
		return 1;
	case Qclonus:
		devdir(c, c->qid, tlsnames[TYPE(c->qid)], 0, eve, 0555, dp);
		return 1;
	default:
		ds = dstate[CONV(c->qid)];
		devdir(c, c->qid, tlsnames[TYPE(c->qid)], 0, ds->user, 0660, dp);
		return 1;
	}
	return -1;
}

static Chan*
tlsattach(char *spec)
{
	Chan *c;

	c = devattach('a', spec);
	c->qid.path = QID(0, Qtopdir)|CHDIR;
	c->qid.vers = 0;
	return c;
}

static int
tlswalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, tlsgen);
}

static void
tlsstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, tlsgen);
}

static void
closedown(Chan *c, TlsRec *tr)
{
	lock(&dslock);
	if(--tr->ref > 0) {
		unlock(&dslock);
		return;
	}
	dstate[CONV(c->qid)] = nil;
	unlock(&dslock);

	tlshangup(tr);
	if(tr->c)
		cclose(tr->c);
	free(tr->in.sec);
	free(tr->in.new);
	free(tr->out.sec);
	free(tr->out.new);
	free(tr);
}

static Chan*
tlsopen(Chan *c, int omode)
{
	TlsRec *tr, **pp;
	int t, perm;

	perm = 0;
	omode &= 3;
	switch(omode) {
	case OREAD:
		perm = 4;
		break;
	case OWRITE:
		perm = 2;
		break;
	case ORDWR:
		perm = 6;
		break;
	}

	t = TYPE(c->qid);
	switch(t) {
	default:
		panic("tlsopen");
	case Qtopdir:
	case Qprotodir:
	case Qconvdir:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qclonus:
		tr = dsclone(c);
		if(tr == nil)
			error(Enodev);
		break;
	case Qctl:
	case Qdata:
	case Qhand:
		if(waserror()) {
			unlock(&dslock);
			nexterror();
		}
		lock(&dslock);
		pp = &dstate[CONV(c->qid)];
		tr = *pp;
		if(tr == nil)
			dsnew(c, pp);
		else {
			if((perm & (tr->perm>>6)) != perm
			   && (strcmp(up->user, tr->user) != 0
			     || (perm & tr->perm) != perm))
				error(Eperm);

			tr->ref++;
		}
		unlock(&dslock);
		poperror();
		if(t == Qhand){
			if(waserror()){
				qunlock(&tr->in.io);
				closedown(c, tr);
			}
			qlock(&tr->in.io);
			if(tr->handq != nil)
				error(Einuse);
//ZZZ what is the correct buffering here?
			tr->handq = qopen(2 * MaxRecLen, 0, nil, nil);
			if(tr->handq == nil)
				error("can't allocate handshake queue");
			qunlock(&tr->in.io);
			poperror();
		}
		break;
	case Qencalgs:
	case Qhashalgs:
		if(omode != OREAD)
			error(Eperm);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
tlswstat(Chan *c, char *dp)
{
	Dir d;
	TlsRec *tr;

	convM2D(dp, &d);

	tr = dstate[CONV(c->qid)];
	if(tr == nil)
		error(Ebadusefd);
	if(strcmp(tr->user, up->user) != 0)
		error(Eperm);

	memmove(tr->user, d.uid, NAMELEN);
	tr->perm = d.mode;
}

static void
tlsclose(Chan *c)
{
	TlsRec *tr;
	int t;

	t = TYPE(c->qid);
	switch(t) {
	case Qctl:
	case Qdata:
	case Qhand:
		if((c->flag & COPEN) == 0)
			break;

		tr = dstate[CONV(c->qid)];
		if(tr == nil)
			break;

		if(t == Qhand){
			qlock(&tr->in.io);
			if(tr->handq != nil){
				qfree(tr->handq);
				tr->handq = nil;
			}
			qunlock(&tr->in.io);
		}
		closedown(c, tr);
		break;
	}
}

/*
 *  make sure we have at least 'n' bytes in list 'l'
 */
static void
ensure(TlsRec *s, Block **l, int n)
{
	int sofar, i;
	Block *b, *bl;

	sofar = 0;
	for(b = *l; b; b = b->next){
		sofar += BLEN(b);
		if(sofar >= n)
			return;
		l = &b->next;
	}

	while(sofar < n){
		bl = devtab[s->c->type]->bread(s->c, Maxdmsg, 0);
		if(bl == 0)
			error(Ehungup);
		*l = bl;
		i = 0;
		for(b = bl; b; b = b->next){
			i += BLEN(b);
			l = &b->next;
		}
		if(i == 0)
			error(Ehungup);
		sofar += i;
	}
}

/*
 *  copy 'n' bytes from 'l' into 'p' and free
 *  the bytes in 'l'
 */
static void
consume(Block **l, uchar *p, int n)
{
	Block *b;
	int i;

	for(; *l && n > 0; n -= i){
		b = *l;
		i = BLEN(b);
		if(i > n)
			i = n;
		memmove(p, b->rp, i);
		b->rp += i;
		p += i;
		if(BLEN(b) < 0)
			panic("consume");
		if(BLEN(b))
			break;
		*l = b->next;
		freeb(b);
	}
}

/*
 *  give back n bytes
 */
static void
regurgitate(TlsRec *s, uchar *p, int n)
{
	Block *b;

	if(n <= 0)
		return;
	b = s->unprocessed;
	if(s->unprocessed == nil || b->rp - b->base < n) {
		b = allocb(n);
		memmove(b->wp, p, n);
		b->wp += n;
		b->next = s->unprocessed;
		s->unprocessed = b;
	} else {
		b->rp -= n;
		memmove(b->rp, p, n);
	}
}

/*
 *  remove at most n bytes from the queue, if discard is set
 *  dump the remainder
 */
static Block*
qremove(Block **l, int n, int discard)
{
	Block *nb, *b, *first;
	int i;

	first = *l;
	for(b = first; b; b = b->next){
		i = BLEN(b);
		if(i == n){
			if(discard){
				freeblist(b->next);
				*l = 0;
			} else
				*l = b->next;
			b->next = 0;
			return first;
		} else if(i > n){
			i -= n;
			if(discard){
				freeblist(b->next);
				b->wp -= i;
				*l = 0;
			} else {
				nb = allocb(i);
				memmove(nb->wp, b->rp+n, i);
				nb->wp += i;
				b->wp -= i;
				nb->next = b->next;
				*l = nb;
			}
			b->next = 0;
			if(BLEN(b) < 0)
				panic("qremove");
			return first;
		} else
			n -= i;
		if(BLEN(b) < 0)
			panic("qremove");
	}
	*l = 0;
	return first;
}

/*
 * read and process one tls record layer message
 * must be called with tr->in.io held
 */
static void
tlsrecread(TlsRec *tr)
{
	OneWay *volatile in;
	Block *volatile b;
	uchar *p, seq[8], header[RecHdrLen], hmac[MD5dlen];
	int volatile nconsumed;
	int len, type, ver;

	nconsumed = 0;
	if(waserror()){
		if(strcmp(up->error, Eintr) == 0)
			regurgitate(tr, header, nconsumed);
		nexterror();
	}
	ensure(tr, &tr->unprocessed, RecHdrLen);
	consume(&tr->unprocessed, header, RecHdrLen);
	nconsumed = RecHdrLen;
	type = header[0];
	ver = get16(header+1);
	len = get16(header+3);
	if(ver != tr->version && (tr->verset || ver < MinProtoVersion || ver > MaxProtoVersion))
		rcvError(tr, EProtocolVersion, "invalid version in record layer");
	if(len <= 0)
		rcvError(tr, EIllegalParameter, "invalid length in record layer");
	if(len > MaxRecLen)
		rcvError(tr, ERecordOverflow, "record message too long");
	ensure(tr, &tr->unprocessed, len);
	nconsumed = 0;
	poperror();

	/*
	 * If an Eintr happens after this, we'll get out of sync.
	 * Make sure nothing we call can sleep.
	 * Errors are ok, as they kill the connection.
	 * Luckily, allocb won't sleep, it'll just error out.
	 */
	b = qremove(&tr->unprocessed, len, 0);

	in = &tr->in;
	if(waserror()){
//ZZZ kill the connection?
		qunlock(&in->seclock);
		if(b != nil)
			freeb(b);
		nexterror();
	}
	qlock(&in->seclock);
	b = pullupblock(b, len);
	p = b->rp;
	if(in->sec != nil) {
		if(len <= in->sec->maclen)
			rcvError(tr, EDecodeError, "record message too short for mac");
		rc4(&in->sec->rc4, p, len);
		len -= in->sec->maclen;

		/* update length */
		put16(header+3, len);
		put64(seq, in->seq);
		in->seq++;
		(*tr->packMac)(in->sec, in->sec->mackey, seq, header, p, len, hmac);
		if(memcmp(hmac, p+len, in->sec->maclen) != 0)
			rcvError(tr, EBadRecordMac, "record mac mismatch");
	}
	qunlock(&in->seclock);
	poperror();
	if(len <= 0)
		rcvError(tr, EDecodeError, "runt record message");

	switch(type) {
	default:
		rcvError(tr, EIllegalParameter, "invalid record message 0x%x", type);
		return;
	case RChangeCipherSpec:
		if(len != 1 || p[0] != 1)
			rcvError(tr, EDecodeError, "invalid change cipher spec");
		qlock(&in->seclock);
		if(in->new == nil){
			qunlock(&in->seclock);
			rcvError(tr, EUnexpectedMessage, "unexpected change cipher spec");
		}
		free(in->sec);
		in->sec = in->new;
		in->new = nil;
		in->seq = 0;
		qunlock(&in->seclock);
		break;
	case RAlert:
		if(len != 2)
			rcvError(tr, EDecodeError, "invalid alert");
		if(p[0] == 1) {
			if(p[1] == ECloseNotify) {
				rcvError(tr, ECloseNotify, "remote close");
				tlsSetState(tr, SRemoteClosed);
			}
			/*
			 * ignore EUserCancelled, it's meaningless
			 * need to handle ENoRenegotiation
			 */
		} else {
			rcvAlert(tr, p[1]);
		}
		break;
	case RHandshake:
		/*
		 * don't worry about dropping the block
		 * qbwrite always queues even if flow controlled and interrupted.
		 *
		 * if there isn't any handshaker, ignore the request,
		 * but notify the other side we are doing so.
		 */
		if(tr->handq != nil){
			qbwrite(tr->handq, b);
			b = nil;
		}else if(tr->verset && tr->version != SSL3Version)
			sendAlert(tr, ENoRenegotiation);
		break;
	case RApplication:
//ZZZ race on state
		if(tr->state != SOpen)
			rcvError(tr, EUnexpectedMessage, "application message received before handshake completed");
		tr->processed = b;
		b = nil;
		break;
	}
	if(b != nil)
		freeb(b);
}

/*
 *  We can't let Eintr's lose data since the program
 *  doing the read may be able to handle it.  The only
 *  places Eintr is possible is during the read's in consume.
 *  Therefore, we make sure we can always put back the bytes
 *  consumed before the last ensure.
 */
static Block*
tlsbread(Chan *c, long n, ulong offset)
{
	TlsRec *volatile tr;
	Block *b;

	switch(TYPE(c->qid)) {
	default:
		return devbread(c, n, offset);
	case Qhand:
	case Qdata:
		break;
	}

	tr = dstate[CONV(c->qid)];
	if(tr == nil)
		panic("tlsbread");

	if(waserror()){
		qunlock(&tr->in.io);
		nexterror();
	}
	qlock(&tr->in.io);
	if(TYPE(c->qid) == Qdata){
//ZZZ race setting state
		if(tr->state != SOpen)
			error(Ebadusefd);
		while(tr->processed == nil)
			tlsrecread(tr);

		/* return at most what was asked for */
		b = qremove(&tr->processed, n, 0);
		qunlock(&tr->in.io);
		poperror();
	}else{
//ZZZ race setting state
		while(tr->state == SHandshake && !qcanread(tr->handq))
			tlsrecread(tr);
		qunlock(&tr->in.io);
		poperror();
		b = qbread(tr->handq, n);
	}

	return b;
}

static long
tlsread(Chan *c, void *a, long n, vlong off)
{
	Block *volatile b;
	Block *nb;
	uchar *va;
	int i;
	char buf[16];
	ulong offset = off;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, tlsgen);

	switch(TYPE(c->qid)) {
	default:
		error(Ebadusefd);
	case Qctl:
		snprint(buf, sizeof(buf), "%lud", CONV(c->qid));
		return readstr(offset, a, n, buf);
	case Qdata:
	case Qhand:
		b = tlsbread(c, n, offset);
		break;
	case Qencalgs:
		return readstr(offset, a, n, encalgs);
	case Qhashalgs:
		return readstr(offset, a, n, hashalgs);
	}

	if(waserror()){
		freeblist(b);
		nexterror();
	}

	n = 0;
	va = a;
	for(nb = b; nb; nb = nb->next){
		i = BLEN(nb);
		memmove(va+n, nb->rp, i);
		n += i;
	}

	freeblist(b);
	poperror();

	return n;
}

/*
 *  this algorithm doesn't have to be great since we're just
 *  trying to obscure the block fill
 */
static void
randfill(uchar *buf, int len)
{
	while(len-- > 0)
		*buf++ = nrand(256);
}

/*
 *  write a block in tls records
 */
static void
tlsrecwrite(TlsRec *tr, int type, Block *b)
{
	Block *volatile bb;
	Block *nb;
	uchar *p, seq[8];
	OneWay *volatile out;
	int n, maclen;

	out = &tr->out;
	bb = b;
	if(waserror()){
		qunlock(&out->io);
		if(bb != nil)
			freeb(bb);
		nexterror();
	}
	qlock(&out->io);

	while(bb != nil){
//ZZZ race on state
		if(tr->state != SHandshake && tr->state != SOpen)
			error(Ebadusefd);

		/*
		 * get at most one maximal record's input,
		 * with padding on the front for header and back for mac
		 */
		if(waserror()){
			qunlock(&out->seclock);
			nexterror();
		}
		qlock(&out->seclock);
		maclen = 0;
		if(out->sec != nil)
			maclen = out->sec->maclen;
		n = BLEN(bb);
		if(n > MaxRecLen){
			n = MaxRecLen;
			nb = allocb(n + RecHdrLen + maclen);
			memmove(nb->wp + RecHdrLen, bb->rp, n);
			nb->wp += n + RecHdrLen;
			bb->rp += n;
		}else{
			/*
			 * carefully reuse bb so it will get freed if we're out of memory
			 */
			bb = padblock(bb, RecHdrLen);
			if(maclen)
				nb = padblock(bb, -maclen);
			else
				nb = bb;
			bb = nil;
		}

		p = nb->rp;
		p[0] = type;
		put16(p+1, tr->version);
		put16(p+3, n);

		if(out->sec != nil){
			put64(seq, out->seq);
			out->seq++;
			(*tr->packMac)(out->sec, out->sec->mackey, seq, p, p + RecHdrLen, n, nb->wp);
			b->wp += maclen;
			n += maclen;

			/* update length */
			put16(p+3, n);

			/* encrypt */
			rc4(&out->sec->rc4, p+RecHdrLen, n);
		}
		qunlock(&out->seclock);
		poperror();

		/*
		 * if bwrite error's, we assume the block is queued.
		 * if not, we're out of sync with the receiver and will not recover.
		 */
		devtab[tr->c->type]->bwrite(tr->c, nb, 0);
	}
	qunlock(&out->io);
	poperror();
}

static long
tlsbwrite(Chan *c, Block *b, ulong offset)
{
	TlsRec *tr;
	ulong n;

	n = BLEN(b);

	tr = dstate[CONV(c->qid)];
	if(tr == nil)
		panic("tlsbread");

	switch(TYPE(c->qid)) {
	default:
		return devbwrite(c, b, offset);
	case Qhand:
//ZZZ race setting state
//		if(tr->state != SHandshake && tr->state != SOpen)
//			error(Ebadusefd);
		tlsrecwrite(tr, RHandshake, b);
		break;
	case Qdata:
//ZZZ race setting state
		if(tr->state != SOpen)
			error(Ebadusefd);
		tlsrecwrite(tr, RApplication, b);
		break;
	}

	return n;
}

typedef struct Hashalg Hashalg;
struct Hashalg
{
	char	*name;
	int	maclen;
	void	(*initkey)(Hashalg *, int, Secret *, uchar*);
};

static void
initmd5key(Hashalg *ha, int version, Secret *s, uchar *p)
{
	s->maclen = ha->maclen;
	if(version == SSL3Version)
		s->mac = sslmac_md5;
	else
		s->mac = hmac_md5;
	memmove(s->mackey, p, ha->maclen);
}

static Hashalg hashtab[] =
{
	{ "clear" },
	{ "md5", MD5dlen, initmd5key, },
	{ 0 }
};

static Hashalg*
parsehashalg(char *p)
{
	Hashalg *ha;

	for(ha = hashtab; ha->name; ha++)
		if(strcmp(p, ha->name) == 0)
			return ha;
	error("unsupported hash algorithm");
	return nil;
}

typedef struct Encalg Encalg;
struct Encalg
{
	char	*name;
	int	keylen;
	int	ivlen;
	void	(*initkey)(Encalg *ea, Secret *, uchar*, uchar*);
};

static void
initRC4key(Encalg *ea, Secret *s, uchar *p, uchar *)
{
	setupRC4state(&s->rc4, p, ea->keylen);
}

static Encalg encrypttab[] =
{
	{ "clear" },
	{ "rc4_128", 128/8, 0, initRC4key, },
	{ 0 }
};

static Encalg*
parseencalg(char *p)
{
	Encalg *ea;

	for(ea = encrypttab; ea->name; ea++)
		if(strcmp(p, ea->name) == 0)
			return ea;
	error("unsupported encryption algorithm");
	return nil;
}

static long
tlswrite(Chan *c, void *a, long n, vlong off)
{
	Encalg *ea;
	Hashalg *ha;
	TlsRec *volatile tr;
	Secret *tos, *toc;
	Block *volatile b;
	Cmdbuf *volatile cb;
	int m;
	char *p, *e;
	uchar *volatile x;
	ulong offset = off;

	tr = dstate[CONV(c->qid)];
	if(tr == nil)
		panic("tlswrite");

	switch(TYPE(c->qid)){
	case Qdata:
	case Qhand:
		p = a;
		e = p + n;
		do{
			m = e - p;
			if(m > MaxRecLen)
				m = MaxRecLen;

			b = allocb(m);
			if(waserror()){
				freeb(b);
				nexterror();
			}
			memmove(b->wp, p, m);
			poperror();
			b->wp += m;

			tlsbwrite(c, b, offset);

			p += m;
		}while(p < e);
		return n;
	case Qctl:
		break;
	default:
		error(Ebadusefd);
		return -1;
	}

	cb = parsecmd(a, n);
	if(waserror()){
		free(cb);
		nexterror();
	}
	if(cb->nf < 1)
		error("short control request");

	/* mutex with operations using what we're about to change */
//ZZZ check this locking
	if(waserror()){
		qunlock(&tr->in.seclock);
		qunlock(&tr->out.seclock);
		nexterror();
	}
	qlock(&tr->in.seclock);
	qlock(&tr->out.seclock);

	if(strcmp(cb->f[0], "fd") == 0){
		if(cb->nf != 3)
			error("usage: fd n version");
		if(tr->c != nil)
			error(Einuse);
		m = strtol(cb->f[2], nil, 0);
		if(m < MinProtoVersion || m > MaxProtoVersion)
			error("unsupported version");
		tr->c = buftochan(cb->f[1]);
		tr->version = m;
		tlsSetState(tr, SHandshake);
	}else if(strcmp(cb->f[0], "version") == 0){
		if(cb->nf != 2)
			error("usage: version n");
		if(tr->c == nil)
			error("must set fd before version");
		if(tr->verset)
			error("version already set");
		m = strtol(cb->f[1], nil, 0);
		if(m == SSL3Version)
			tr->packMac = sslPackMac;
		else if(m == TLSVersion)
			tr->packMac = tlsPackMac;
		else
			error("unsupported version");
		tr->verset = 1;
		tr->version = m;
	}else if(strcmp(cb->f[0], "opened") == 0){
		if(cb->nf != 1)
			error("usage: opened");
		tlsSetState(tr, SOpen);
	}else if(strcmp(cb->f[0], "alert") == 0){
		if(cb->nf != 2)
			error("usage: alert n");
		if(tr->c == nil)
			error("must set fd before sending alerts");
		m = strtol(cb->f[1], nil, 0);

		qunlock(&tr->in.seclock);
		qunlock(&tr->out.seclock);
		poperror();
		free(cb);
		poperror();

		sendAlert(tr, m);

		return n;
	}else if(strcmp(cb->f[0], "changecipher") == 0){
		if(cb->nf != 1)
			error("usage: changecipher");
		if(tr->out.new == nil)
			error("can't change cipher spec without setting secret");

		toc = tr->out.new;
		tr->out.new = nil;

//ZZZ minor race; worth fixing?
		qunlock(&tr->in.seclock);
		qunlock(&tr->out.seclock);
		poperror();
		free(cb);
		poperror();
	
		b = allocb(1);
		*b->wp++ = 1;
		tlsrecwrite(tr, RChangeCipherSpec, b);

		qlock(&tr->out.seclock);
		free(tr->out.sec);
		tr->out.sec = toc;
		qunlock(&tr->out.seclock);
		return n;
	}else if(strcmp(cb->f[0], "secret") == 0){
		if(cb->nf != 5)
			error("usage: secret hashalg encalg isclient secretdata");
		if(tr->c == nil || !tr->verset)
			error("must set fd and version before secrets");

		if(tr->in.new != nil){
			free(tr->in.new);
			tr->in.new = nil;
		}
		if(tr->out.new != nil){
			free(tr->out.new);
			tr->out.new = nil;
		}

		ha = parsehashalg(cb->f[1]);
		ea = parseencalg(cb->f[2]);

		p = cb->f[4];
		m = (strlen(p)*3)/2;
		x = smalloc(m);
		if(waserror()){
			free(x);
			nexterror();
		}
		m = dec64(x, m, p, strlen(p));
		if(m < 2 * ha->maclen + 2 * ea->keylen + 2 * ea->ivlen)
			error("not enough secret data provided");

		tos = smalloc(sizeof(Secret));
		toc = smalloc(sizeof(Secret));
		if(ha->initkey){
			(*ha->initkey)(ha, tr->version, tos, &x[0]);
			(*ha->initkey)(ha, tr->version, toc, &x[ha->maclen]);
		}
		if(ea->initkey){
			(*ea->initkey)(ea, tos, &x[2 * ha->maclen], &x[2 * ha->maclen + 2 * ea->keylen]);
			(*ea->initkey)(ea, toc, &x[2 * ha->maclen + ea->keylen], &x[2 * ha->maclen + 2 * ea->keylen + ea->ivlen]);
		}
		if(strtol(cb->f[3], nil, 0) == 0){
			tr->in.new = tos;
			tr->out.new = toc;
		}else{
			tr->in.new = toc;
			tr->out.new = tos;
		}

		free(x);
		poperror();
	} else
		error(Ebadarg);

	qunlock(&tr->in.seclock);
	qunlock(&tr->out.seclock);
	poperror();
	free(cb);
	poperror();

	return n;
}

static void
tlsinit(void)
{
	struct Encalg *e;
	struct Hashalg *h;
	int n;
	char *cp;

	if((dstate = smalloc(sizeof(TlsRec*) * maxdstate)) == 0)
		panic("tlsinit");

	n = 1;
	for(e = encrypttab; e->name != nil; e++)
		n += strlen(e->name) + 1;
	cp = encalgs = smalloc(n);
	for(e = encrypttab;;){
		strcpy(cp, e->name);
		cp += strlen(e->name);
		e++;
		if(e->name == nil)
			break;
		*cp++ = ' ';
	}
	*cp = 0;

	n = 1;
	for(h = hashtab; h->name != nil; h++)
		n += strlen(h->name) + 1;
	cp = hashalgs = smalloc(n);
	for(h = hashtab;;){
		strcpy(cp, h->name);
		cp += strlen(h->name);
		h++;
		if(h->name == nil)
			break;
		*cp++ = ' ';
	}
	*cp = 0;
}

Dev tlsdevtab = {
	'a',
	"tls",

	devreset,
	tlsinit,
	tlsattach,
	devclone,
	tlswalk,
	tlsstat,
	tlsopen,
	devcreate,
	tlsclose,
	tlsread,
	tlsbread,
	tlswrite,
	tlsbwrite,
	devremove,
	tlswstat,
};

/* get channel associated with an fd */
static Chan*
buftochan(char *p)
{
	Chan *c;
	int fd;

	if(p == 0)
		error(Ebadarg);
	fd = strtoul(p, 0, 0);
	if(fd < 0)
		error(Ebadarg);
	c = fdtochan(fd, -1, 0, 1);	/* error check and inc ref */
	return c;
}

static void
sendAlert(TlsRec *tr, int err)
{
	Block *b;
	int i, fatal;

	fatal = 1;
	for(i=0; i < nelem(tlserrs); i++) {
		if(tlserrs[i].err == err) {
			if(tr->version == SSL3Version)
				err = tlserrs[i].sslerr;
			else
				err = tlserrs[i].tlserr;
			fatal = tlserrs[i].fatal;
			break;
		}
	}

	b = allocb(2);
	*b->wp++ = fatal + 1;
	*b->wp++ = err;
	tlsrecwrite(tr, RAlert, b);
//ZZZ race on state
	if(fatal)
		tlsSetState(tr, SError);
}

static void
rcvAlert(TlsRec *tr, int err)
{
	char *s;
	int i, fatal;

	s = "unknown error";
	fatal = 1;
	for(i=0; i < nelem(tlserrs); i++){
		if(tlserrs[i].err == err){
			s = tlserrs[i].msg;
			fatal = tlserrs[i].fatal;
			break;
		}
	}
//ZZZ need to kill session if fatal error
	if(fatal)
		tlsSetState(tr, SError);
	error(s);
}

static void
rcvError(TlsRec *tr, int err, char *fmt, ...)
{
	char msg[ERRLEN];
	va_list arg;

	sendAlert(tr, err);
	va_start(arg, fmt);
	strcpy(msg, "tls local %s");
	doprint(strchr(msg, '\0'), msg+sizeof(msg), fmt, arg);
	va_end(arg);
	error(msg);
}

static void
tlsSetState(TlsRec *tr, int newstate)
{
	lock(&tr->statelk);
	tr->state = newstate;
	unlock(&tr->statelk);
}

/* hand up a digest connection */
static void
tlshangup(TlsRec *s)
{
	Block *b;

	qlock(&s->in.io);
	for(b = s->processed; b; b = s->processed){
		s->processed = b->next;
		freeb(b);
	}
	if(s->unprocessed != nil){
		freeb(s->unprocessed);
		s->unprocessed = nil;
	}
	s->state = SClosed;
	qunlock(&s->in.io);
}

static TlsRec*
dsclone(Chan *ch)
{
	TlsRec **pp, **ep, **np;
	int newmax;

	if(waserror()) {
		unlock(&dslock);
		nexterror();
	}
	lock(&dslock);
	ep = &dstate[maxdstate];
	for(pp = dstate; pp < ep; pp++) {
		if(*pp == nil) {
			dsnew(ch, pp);
			break;
		}
	}
	if(pp >= ep) {
		if(maxdstate >= Maxdstate) {
			unlock(&dslock);
			poperror();
			return nil;
		}
		newmax = 2 * maxdstate;
		if(newmax > Maxdstate)
			newmax = Maxdstate;
		np = smalloc(sizeof(TlsRec*) * newmax);
		if(np == nil)
			error(Enomem);
		memmove(np, dstate, sizeof(TlsRec*) * maxdstate);
		dstate = np;
		pp = &dstate[maxdstate];
		memset(pp, 0, sizeof(TlsRec*)*(newmax - maxdstate));
		maxdstate = newmax;
		dsnew(ch, pp);
	}
	unlock(&dslock);
	poperror();
	return *pp;
}

static void
dsnew(Chan *ch, TlsRec **pp)
{
	TlsRec *s;
	int t;

	*pp = s = malloc(sizeof(*s));
	if(!s)
		error(Enomem);
	if(pp - dstate >= dshiwat)
		dshiwat++;
	memset(s, 0, sizeof(*s));
	s->state = SClosed;
	s->ref = 1;
	strncpy(s->user, up->user, sizeof(s->user));
	s->perm = 0660;
	t = TYPE(ch->qid);
	if(t == Qclonus)
		t = Qctl;
	ch->qid.path = QID(pp - dstate, t);
	ch->qid.vers = 0;
}

/*
 * sslmac: mac calculations for ssl 3.0 only; tls 1.0 uses the standard hmac.
 */
static DigestState*
sslmac_x(uchar *p, ulong len, uchar *key, ulong klen, uchar *digest, DigestState *s,
	DigestState*(*x)(uchar*, ulong, uchar*, DigestState*), int xlen, int padlen)
{
	int i;
	uchar pad[48], innerdigest[20];

	if(xlen > sizeof(innerdigest)
	|| padlen > sizeof(pad))
		return nil;

	if(klen>64)
		return nil;

	/* first time through */
	if(s == nil){
		for(i=0; i<padlen; i++)
			pad[i] = 0x36;
		s = (*x)(key, klen, nil, nil);
		s = (*x)(pad, padlen, nil, s);
		if(s == nil)
			return nil;
	}

	s = (*x)(p, len, nil, s);
	if(digest == nil)
		return s;

	/* last time through */
	for(i=0; i<padlen; i++)
		pad[i] = 0x5c;
	(*x)(nil, 0, innerdigest, s);
	s = (*x)(key, klen, nil, nil);
	s = (*x)(pad, padlen, nil, s);
	(*x)(innerdigest, xlen, digest, s);
	return nil;
}

static DigestState*
sslmac_sha1(uchar *p, ulong len, uchar *key, ulong klen, uchar *digest, DigestState *s)
{
	return sslmac_x(p, len, key, klen, digest, s, sha1, SHA1dlen, 40);
}

static DigestState*
sslmac_md5(uchar *p, ulong len, uchar *key, ulong klen, uchar *digest, DigestState *s)
{
	return sslmac_x(p, len, key, klen, digest, s, md5, MD5dlen, 48);
}

static void
sslPackMac(Secret *sec, uchar *mackey, uchar *seq, uchar *header, uchar *body, int len, uchar *mac)
{
	DigestState *s;
	uchar buf[11];

	memmove(buf, seq, 8);
	buf[8] = header[0];
	buf[9] = header[3];
	buf[10] = header[4];

	s = (*sec->mac)(buf, 11, mackey, sec->maclen, 0, 0);
	(*sec->mac)(body, len, mackey, sec->maclen, mac, s);
}

static void
tlsPackMac(Secret *sec, uchar *mackey, uchar *seq, uchar *header, uchar *body, int len, uchar *mac)
{
	DigestState *s;
	uchar buf[13];

	memmove(buf, seq, 8);
	memmove(&buf[8], header, 5);

	s = (*sec->mac)(buf, 13, mackey, sec->maclen, 0, 0);
	(*sec->mac)(body, len, mackey, sec->maclen, mac, s);
}

static void
put32(uchar *p, u32int x)
{
	p[0] = x>>24;
	p[1] = x>>16;
	p[2] = x>>8;
	p[3] = x;
}

static void
put64(uchar *p, vlong x)
{
	put32(p, (u32int)(x >> 32));
	put32(p+4, (u32int)x);
}

static void
put16(uchar *p, int x)
{
	p[0] = x>>8;
	p[1] = x;
}

static u32int
get32(uchar *p)
{
	return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

static int
get16(uchar *p)
{
	return (p[0]<<8)|p[1];
}
