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

#define NOSPOOKS 1

typedef struct OneWay OneWay;
enum
{

	/* encryption algorithms */
	Noencryption=	0,
	DESCBC=		1,
	DESECB=		2,
	RC4=		3
};

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
struct OneWay
{
	QLock		q;		/* locks io access */
	QLock		ctlq;		/* locks one-way paramaters */

	ulong		seq;
	int		keyed;		/* have key, waiting for cipher enable */

	int		protected;	/* cipher is enabled */
	RC4state	rc4;
	uchar		mackey[64];
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
	void		(*packMac)(TlsRec*, uchar*, uchar*, uchar*, uchar*, int, uchar*);
	DigestState	*(*mac)(uchar*, ulong, uchar*, ulong, uchar*, DigestState*);
	int		maclen;

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

static	Lock	dslock;
static	int	dshiwat;
static	int	maxdstate = 128;
static	TlsRec** dstate;
static	char	*encalgs;
static	char	*hashalgs;

enum
{
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
static void	setsecret(OneWay*, uchar*, int);
static Block*	encryptb(TlsRec*, Block*, int);
static Block*	decryptb(TlsRec*, Block*);
static Block*	digestb(TlsRec*, Block*, int);
static void	checkdigestb(TlsRec*, Block*);
static Chan*	buftochan(char*);
static void	tlshangup(TlsRec*);
static TlsRec*	dsclone(Chan *c);
static void	dsnew(Chan *c, TlsRec **);
static void	put64(uchar *p, vlong x);
static void	put32(uchar *p, u32int);
static void	put24(uchar *p, int);
static void	put16(uchar *p, int);
static u32int	get32(uchar *p);
static int	get16(uchar *p);
static void	tlsAlert(TlsRec *tr, int err);
static void	tlsError(TlsRec *tr, int err, char *msg, ...);
#pragma	varargck	argpos	tlsError	3

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

static Chan*
tlsopen(Chan *c, int omode)
{
	TlsRec *s, **pp;
	int perm;

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

	switch(TYPE(c->qid)) {
	default:
		panic("tlsopen");
	case Qtopdir:
	case Qprotodir:
	case Qconvdir:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qclonus:
		s = dsclone(c);
		if(s == 0)
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
		s = *pp;
		if(s == 0)
			dsnew(c, pp);
		else {
			if((perm & (s->perm>>6)) != perm
			   && (strcmp(up->user, s->user) != 0
			     || (perm & s->perm) != perm))
				error(Eperm);

			s->ref++;
		}
		unlock(&dslock);
		poperror();
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
	TlsRec *s;

	convM2D(dp, &d);

	s = dstate[CONV(c->qid)];
	if(s == nil)
		error(Ebadusefd);
	if(strcmp(s->user, up->user) != 0)
		error(Eperm);

	memmove(s->user, d.uid, NAMELEN);
	s->perm = d.mode;
}

static void
tlsclose(Chan *c)
{
	TlsRec *s;

	switch(TYPE(c->qid)) {
	case Qctl:
	case Qdata:
	case Qhand:
		if((c->flag & COPEN) == 0)
			break;

		s = dstate[CONV(c->qid)];
		if(s == nil)
			break;

		lock(&dslock);
		if(--s->ref > 0) {
			unlock(&dslock);
			break;
		}
		dstate[CONV(c->qid)] = nil;
		unlock(&dslock);

		tlshangup(s);
		if(s->c)
			cclose(s->c);
		free(s);
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
 * must be called with tr->in.q held
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
		tlsError(tr, EProtocolVersion, "invalid version in record layer");
	if(len <= 0)
		tlsError(tr, EIllegalParameter, "invalid length in record layer");
	if(len > MaxRecLen)
		tlsError(tr, ERecordOverflow, "record message too long");
	ensure(tr, &tr->unprocessed, len);
	nconsumed = 0;

	/*
	 *  if an Eintr happens after this, we're screwed.  Make
	 *  sure nothing we call can sleep.  Luckily, allocb
	 *  won't sleep, it'll just error out.
	 *  grab the next message and decode/decrypt it
	 */
	b = qremove(&tr->unprocessed, len, 0);

	in = &tr->in;
	if(waserror()){
		qunlock(&in->ctlq);
		if(b != nil)
			freeb(b);
		nexterror();
	}
	qlock(&in->ctlq);
	b = pullupblock(b, len);
	p = b->rp;
	if(in->protected) {
		if(len <= tr->maclen)
			tlsError(tr, EDecodeError, "record message too short for mac");
		rc4(&in->rc4, p, len);
		len -= tr->maclen;

		/* update length */
		put16(header+3, len);
		put64(seq, in->seq);
		in->seq++;
		(*tr->packMac)(tr, in->mackey, seq, header, p, len, hmac);
		if(memcmp(hmac, p+len, tr->maclen) != 0)
			tlsError(tr, EBadRecordMac, "record mac mismatch");
	}
	qunlock(&tr->in.ctlq);
	poperror();
	if(len <= 0)
		tlsError(tr, EDecodeError, "runt record message");

	switch(type) {
	default:
		tlsError(tr, EIllegalParameter, "invalid record message 0x%x", type);
		return;
	case RChangeCipherSpec:
		if(len != 1 || p[0] != 1)
			tlsError(tr, EHandshakeFailure, "invalid change cipher spec");
		qlock(&in->ctlq);
		if(!tr->in.keyed){
			qunlock(&in->ctlq);
			tlsError(tr, EUnexpectedMessage, "unexpected change cipher spec");
		}
		tr->in.keyed = 0;
		tr->in.protected = 1;
		tr->in.seq = 0;
		qunlock(&in->ctlq);
		break;
	case RAlert:
		if(len != 2)
			tlsError(tr, EDecodeError, "invalid alert");
		if(p[0] == 1) {
			if(p[1] == ECloseNotify) {
				tlsError(tr, ECloseNotify, "remote close");
				tlsSetState(tr, SRemoteClosed);
			}
		} else {
			tlsSetState(tr, SError);
			tlsAlert(tr, p[1]);
		}
		break;
	case RHandshake:
		/*
		 * don't worry about dropping the block
		 * qbwrite always queue it even if flow controlled and interrupted.
		 */
		if(tr->handq != nil){
			qbwrite(tr->handq, b);
			b = nil;
		}
		break;
	case RApplication:
//ZZZ race on state
		if(tr->state != SOpen)
			tlsError(tr, EUnexpectedMessage, "application message received before handshake completed");
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
		qunlock(&tr->in.q);
		nexterror();
	}
	qlock(&tr->in.q);
	if(TYPE(c->qid) == Qdata){
//ZZZ race setting state
		if(tr->state != SOpen)
			error(Ebadusefd);
		while(tr->processed == nil)
			tlsrecread(tr);

		/* return at most what was asked for */
		b = qremove(&tr->processed, n, 0);
		qunlock(&tr->in.q);
		poperror();
	}else{
//ZZZ race setting state
		while(tr->state == SHandshake && !qcanread(tr->handq))
			tlsrecread(tr);
		qunlock(&tr->in.q);
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
	char buf[128];
	ulong offset = off;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, tlsgen);

	switch(TYPE(c->qid)) {
	default:
		error(Ebadusefd);
	case Qctl:
		sprint(buf, "%lud", CONV(c->qid));
		return readstr(offset, a, n, buf);
	case Qdata:
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
		qunlock(&out->q);
		if(bb != nil)
			freeb(bb);
		nexterror();
	}
	qlock(&out->q);

	while(bb != nil){
//ZZZ race on state
		if(tr->state != SHandshake && tr->state != SOpen)
			error(Ebadusefd);

		/*
		 * get at most one maximal record's input,
		 * with padding on the front for header and back for mac
		 */
		if(waserror()){
			qunlock(&out->ctlq);
			nexterror();
		}
		qlock(&out->ctlq);
		maclen = 0;
		if(out->protected)
			maclen = tr->maclen;
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

		if(out->protected){
			put64(seq, out->seq);
			out->seq++;
			(*tr->packMac)(tr, out->mackey, seq, p, p + RecHdrLen, n, nb->wp);
			b->wp += maclen;
			n += maclen;

			/* update length */
			put16(p+3, n);

			/* encrypt */
			rc4(&out->rc4, p+RecHdrLen, n);
		}
		qunlock(&out->ctlq);
		poperror();

		/*
		 * if bwrite error's, we assume the block is queued.
		 * if not, we're out of sync with the receiver and will not recover.
		 */
		devtab[tr->c->type]->bwrite(tr->c, nb, 0);
	}
	qunlock(&out->q);
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
		if(tr->state != SHandshake && tr->state != SOpen)
			error(Ebadusefd);
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

static void
setsecret(OneWay *w, uchar *secret, int n)
{
	if(w->secret)
		free(w->secret);

	w->secret = smalloc(n);
	memmove(w->secret, secret, n);
	w->slen = n;
}

/*
 *  128 bit RC4 is the same as n-bit RC4.  However,
 *  we ignore all but the first 128 bits of the key.
 */
static void
initRC4key_128(OneWay *w)
{
	if(w->state){
		free(w->state);
		w->state = 0;
	}

	if(w->slen > 16)
		w->slen = 16;

	w->state = malloc(sizeof(RC4state));
	setupRC4state(w->state, w->secret, w->slen);
}


typedef struct Hashalg Hashalg;
struct Hashalg
{
	char	*name;
	int	diglen;
	DigestState *(*hf)(uchar*, ulong, uchar*, DigestState*);
};

Hashalg hashtab[] =
{
	{ "md5", MD5dlen, md5, },
	{ 0 }
};

static int
parsehashalg(char *p, TlsRec *s)
{
	Hashalg *ha;

	for(ha = hashtab; ha->name; ha++){
		if(strcmp(p, ha->name) == 0){
			s->hf = ha->hf;
			s->diglen = ha->diglen;
			s->state &= ~Sclear;
			s->state |= Sdigesting;
			return 0;
		}
	}
	return -1;
}

typedef struct Encalg Encalg;
struct Encalg
{
	char	*name;
	int	blocklen;
	int	alg;
	void	(*keyinit)(OneWay*);
};

Encalg encrypttab[] =
{
	{ "rc4_128", 1, RC4, initRC4key_128, },
	{ 0 }
};

static int
parseencryptalg(char *p, TlsRec *s)
{
	Encalg *ea;

	for(ea = encrypttab; ea->name; ea++){
		if(strcmp(p, ea->name) == 0){
			s->encryptalg = ea->alg;
			s->blocklen = ea->blocklen;
			(*ea->keyinit)(&s->in);
			(*ea->keyinit)(&s->out);
			s->state &= ~Sclear;
			s->state |= Sencrypting;
			return 0;
		}
	}
	return -1;
}

static long
tlswrite(Chan *c, void *a, long n, vlong off)
{
	TlsRec *volatile tr;
	Block *volatile b;
	int m, t;
	char *p, *np, *e, buf[128];
	uchar *x;
	ulong offset = off;

	tr = dstate[CONV(c->qid)];
	if(tr == nil)
		panic("tlswrite");

	t = TYPE(c->qid);
	if(t == Qdata || t == Qhand){
		p = a;
		e = p + n;
		do {
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
		} while(p < e);
		return n;
	}

	/* mutex with operations using what we're about to change */
	if(waserror()){
		qunlock(&s->in.ctlq);
		qunlock(&s->out.q);
		nexterror();
	}
	qlock(&s->in.ctlq);
	qlock(&s->out.q);

	switch(t){
	default:
		error(Ebadusefd);
	case Qctl:
		break;
	}

	if(n >= sizeof(buf))
		error("arg too long");
	strncpy(buf, a, n);
	buf[n] = 0;
	p = strchr(buf, '\n');
	if(p)
		*p = 0;
	p = strchr(buf, ' ');
	if(p)
		*p++ = 0;

	if(strcmp(buf, "fd") == 0){
		if(s->c != nil)
			error(Einuse);
		s->c = buftochan(p);

		/* default is clear (msg delimiters only) */
		s->state = Sclear;
		s->blocklen = 1;
		s->diglen = 0;
		s->in.mid = 0;
		s->out.mid = 0;
	} else if(strcmp(buf, "alg") == 0 && p != 0){
		s->blocklen = 1;
		s->diglen = 0;

		if(s->c == 0)
			error("must set fd before algorithm");

		s->state = Sclear;
		s->maxpad = s->max = (1<<15) - s->diglen - 1;
		if(strcmp(p, "clear") == 0){
			goto out;
		}

		if(s->in.secret && s->out.secret == 0)
			setsecret(&s->out, s->in.secret, s->in.slen);
		if(s->out.secret && s->in.secret == 0)
			setsecret(&s->in, s->out.secret, s->out.slen);
		if(s->in.secret == 0 || s->out.secret == 0)
			error("algorithm but no secret");

		s->hf = 0;
		s->encryptalg = Noencryption;
		s->blocklen = 1;

		for(;;){
			np = strchr(p, ' ');
			if(np)
				*np++ = 0;

			if(parsehashalg(p, s) < 0)
			if(parseencryptalg(p, s) < 0)
				error("bad algorithm");

			if(np == 0)
				break;
			p = np;
		}

		if(s->hf == 0 && s->encryptalg == Noencryption)
			error("bad algorithm");

		if(s->blocklen != 1){
			s->max = (1<<15) - s->diglen - 1;
			s->max -= s->max % s->blocklen;
			s->maxpad = (1<<14) - s->diglen - 1;
			s->maxpad -= s->maxpad % s->blocklen;
		} else
			s->maxpad = s->max = (1<<15) - s->diglen - 1;
	} else if(strcmp(buf, "secretin") == 0 && p != 0) {
		m = (strlen(p)*3)/2;
		x = smalloc(m);
		n = dec64(x, m, p, strlen(p));
		setsecret(&s->in, x, n);
		free(x);
	} else if(strcmp(buf, "secretout") == 0 && p != 0) {
		m = (strlen(p)*3)/2 + 1;
		x = smalloc(m);
		n = dec64(x, m, p, strlen(p));
		setsecret(&s->out, x, n);
		free(x);
	} else
		error(Ebadarg);

out:
	qunlock(&s->in.ctlq);
	qunlock(&s->out.q);
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

static Block*
encryptb(TlsRec *s, Block *b, int offset)
{
	uchar *p, *ep, *p2, *ip, *eip;
	DESstate *ds;

	switch(s->encryptalg){
	case DESECB:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + offset; p < ep; p += 8)
			block_cipher(ds->expanded, p, 0);
		break;
	case DESCBC:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + offset; p < ep; p += 8){
			p2 = p;
			ip = ds->ivec;
			for(eip = ip+8; ip < eip; )
				*p2++ ^= *ip++;
			block_cipher(ds->expanded, p, 0);
			memmove(ds->ivec, p, 8);
		}
		break;
	case RC4:
		rc4(s->out.state, b->rp + offset, BLEN(b) - offset);
		break;
	}
	return b;
}

static Block*
decryptb(TlsRec *s, Block *bin)
{
	Block *b, **l;
	uchar *p, *ep, *tp, *ip, *eip;
	DESstate *ds;
	uchar tmp[8];
	int i;

	l = &bin;
	for(b = bin; b; b = b->next){
		/* make sure we have a multiple of s->blocklen */
		if(s->blocklen > 1){
			i = BLEN(b);
			if(i % s->blocklen){
				*l = b = pullupblock(b, i + s->blocklen - (i%s->blocklen));
				if(b == 0)
					error("tls encrypted message too short");
			}
		}
		l = &b->next;

		/* decrypt */
		switch(s->encryptalg){
		case DESECB:
			ds = s->in.state;
			ep = b->rp + BLEN(b);
			for(p = b->rp; p < ep; p += 8)
				block_cipher(ds->expanded, p, 1);
			break;
		case DESCBC:
			ds = s->in.state;
			ep = b->rp + BLEN(b);
			for(p = b->rp; p < ep;){
				memmove(tmp, p, 8);
				block_cipher(ds->expanded, p, 1);
				tp = tmp;
				ip = ds->ivec;
				for(eip = ip+8; ip < eip; ){
					*p++ ^= *ip;
					*ip++ = *tp++;
				}
			}
			break;
		case RC4:
			rc4(s->in.state, b->rp, BLEN(b));
			break;
		}
	}
	return bin;
}

static Block*
digestb(TlsRec *s, Block *b, int offset)
{
	uchar *p;
	DigestState ss;
	uchar msgid[4];
	ulong n, h;
	OneWay *w;

	w = &s->out;

	memset(&ss, 0, sizeof(ss));
	h = s->diglen + offset;
	n = BLEN(b) - h;

	/* hash secret + message */
	(*s->hf)(w->secret, w->slen, 0, &ss);
	(*s->hf)(b->rp + h, n, 0, &ss);

	/* hash message id */
	p = msgid;
	n = w->mid;
	*p++ = n>>24;
	*p++ = n>>16;
	*p++ = n>>8;
	*p = n;
	(*s->hf)(msgid, 4, b->rp + offset, &ss);

	return b;
}

static void
checkdigestb(TlsRec *s, Block *bin)
{
	uchar *p;
	DigestState ss;
	uchar msgid[4];
	int n, h;
	OneWay *w;
	uchar digest[128];
	Block *b;

	w = &s->in;

	memset(&ss, 0, sizeof(ss));

	/* hash secret */
	(*s->hf)(w->secret, w->slen, 0, &ss);

	/* hash message */
	h = s->diglen;
	for(b = bin; b; b = b->next){
		n = BLEN(b) - h;
		if(n < 0)
			panic("checkdigestb");
		(*s->hf)(b->rp + h, n, 0, &ss);
		h = 0;
	}

	/* hash message id */
	p = msgid;
	n = w->mid;
	*p++ = n>>24;
	*p++ = n>>16;
	*p++ = n>>8;
	*p = n;
	(*s->hf)(msgid, 4, digest, &ss);

	if(memcmp(digest, bin->rp, s->diglen) != 0)
		error("bad digest");
}

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

/* hand up a digest connection */
static void
tlshangup(TlsRec *s)
{
	Block *b;

	qlock(&s->in.q);
	for(b = s->processed; b; b = s->processed){
		s->processed = b->next;
		freeb(b);
	}
	if(s->unprocessed != nil){
		freeb(s->unprocessed);
		s->unprocessed = nil;
	}
	s->state = Sincomplete;
	qunlock(&s->in.q);
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
	s->state = Sincomplete;
	s->ref = 1;
	strncpy(s->user, up->user, sizeof(s->user));
	s->perm = 0660;
	t = TYPE(ch->qid);
	if(t == Qclonus)
		t = Qctl;
	ch->qid.path = QID(pp - dstate, t);
	ch->qid.vers = 0;
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
