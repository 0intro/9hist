/*
 *  template for making a new device
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	<libcrypt.h>

#include	"devtab.h"

typedef struct OneWay OneWay;
struct OneWay
{
	QLock;

	void		*state;		/* encryption state */
	int		slen;		/* hash data length */
	uchar		*secret;	/* secret */
	ulong		mid;		/* message id */
};

enum
{
	/* connection states */
	Algwait=	0,	/* waiting for user to write algorithm */
	Fdwait=		1,	/* waiting for user to write fd */
	Secretinwait=	2,	/* waiting for user to write input secret */
	Secretoutwait=	3,	/* waiting for user to write output secret */
	Established=	4,
	Closed=		5,

	/* encryption algorithms */
	Noencryption=	0,
	DESCBC=		1,
	DESECB=		2,
};

typedef struct Dstate Dstate;
struct Dstate
{
	Chan	*c;		/* io channel */
	uchar	state;		/* state of connection */
	uchar	encryptalg;	/* encryption algorithm */
	ushort	blocklen;	/* blocking length */

	ushort	diglen;		/* length of digest */
	DigestState *(*hf)(uchar*, ulong, uchar*, DigestState*);	/* hash func */

	/* for SSL format */
	int	max;		/* maximum unpadded data per msg */
	int	maxpad;		/* maximum padded data per msg */

	/* input side */
	OneWay	in;
	Block	*processed;
	Block	*unprocessed;

	/* output side */
	OneWay	out;
};

enum
{
	Maxdmsg=	1<<16,
};

enum{
	Qdir,
	Qclone,
};
Dirtab digesttab[]={
	"ssl",		{Qclone, 0},	0,	0600,
};
#define Ndigesttab (sizeof(digesttab)/sizeof(Dirtab))

/* a circular list of random numbers */
typedef struct
{
	uchar	*rp;
	uchar	*wp;
	uchar	buf[1024];
	uchar	*ep;
} Randq;
Randq randq;

void producerand(void);

static void	ensure(Dstate*, Block**, int);
static void	consume(Block**, uchar*, int);
static void	setsecret(Dstate*, OneWay*, uchar*, int);
static Block*	encryptb(Dstate*, Block*, int);
static Block*	decryptb(Dstate*, Block*);
static Block*	digestb(Dstate*, Block*, int);
static void	checkdigestb(Dstate*, Block*);
static Chan*	buftochan(char*, long);
static void	dighangup(Dstate*);


void
sslreset(void)
{
	randq.ep = randq.buf + sizeof(randq.buf);
	randq.rp = randq.wp = randq.buf;
}

void
sslinit(void)
{
}

Chan *
sslattach(char *spec)
{
	return devattach('D', spec);
}

Chan *
sslclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
sslwalk(Chan *c, char *name)
{
	return devwalk(c, name, digesttab, Ndigesttab, devgen);
}

void
sslstat(Chan *c, char *db)
{
	devstat(c, db, digesttab, Ndigesttab, devgen);
}

Chan *
sslopen(Chan *c, int omode)
{
	Dstate *s;

	switch(c->qid.path & ~CHDIR){
	case Qclone:
		s = smalloc(sizeof(Dstate));
		memset(s, 0, sizeof(*s));
		s->state = Algwait;
		c->aux = s;
		break;
	}
	return devopen(c, omode, digesttab, Ndigesttab, devgen);
}

void
sslcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
sslremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
sslwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
sslclose(Chan *c)
{
	Dstate *s;

	if(c->aux){
		s = c->aux;
		dighangup(s);
		if(s->c)
			close(s->c);
		if(s->in.secret)
			free(s->in.secret);
		if(s->out.secret)
			free(s->out.secret);
		free(s);
	}
}

long
sslread(Chan *c, void *a, long n, ulong offset)
{
	Block *b;

	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, digesttab, Ndigesttab, devgen);
	}

	b = sslbread(c, n, offset);

	if(waserror()){
		freeb(b);
		nexterror();
	}

	n = BLEN(b);
	memmove(a, b->rp, n);
	freeb(b);

	poperror();

	return n;
}

Block*
sslbread(Chan *c, long n, ulong offset)
{
	Dstate *s;
	Block *b;
	uchar count[2];
	int i, len, pad;

	USED(offset);

	s = c->aux;
	if(s == 0 || s->state != Established)
		error(Ebadusefd);

	if(waserror()){
		qunlock(&s->in);
		dighangup(s);
		nexterror();
	}
	qlock(&s->in);

	b = s->processed;
	if(b == 0){
	
		/* read in the whole message */
		s->processed = s->unprocessed;
		s->unprocessed = 0;
		ensure(s, &s->processed, 2);
		consume(&s->processed, count, 2);
		if(count[0] & 0x80){
			len = ((count[0] & 0x7f)<<8) | count[1];
			ensure(s, &s->processed, len);
			pad = 0;
		} else {
			len = ((count[0] & 0x3f)<<8) | count[1];
			ensure(s, &s->processed, len+1);
			consume(&s->processed, count, 1);
			pad = count[0];
		}

		/* trade memory bandwidth for less processing complexity */
		b = s->processed = pullupblock(s->processed, len);

		/* put remainder on unprocessed queue */
		i = BLEN(b);
		if(i > len){
			i -= len;
			s->unprocessed = allocb(i);
			memmove(s->unprocessed->wp, b->rp+len, i);
			s->unprocessed->wp += i;
			b->wp -= i;
		}

		if(s->encryptalg)
			b = decryptb(s, b);
		else {
			if(BLEN(b) < s->diglen)
				error("baddigest");
			checkdigestb(s, b);
			b->rp += s->diglen;
		}

		/* remove pad */
		if(b->wp - b->rp < pad)
			panic("sslbread");
		b->wp -= pad;
		s->processed = b;
	}

	if(BLEN(b) > n){
		b = allocb(n);
		memmove(b->wp, s->processed->rp, n);
		b->wp += n;
		s->processed->rp += n;
	} else 
		s->processed = b->next;

	qunlock(&s->in);
	poperror();

	return b;
}

long
sslwrite(Chan *c, void *a, long n, ulong offset)
{
	Dstate *s;
	Block *b;
	int m;
	char *p, *e, buf[32];

	switch(c->qid.path & ~CHDIR){
	case Qclone:
		break;
	default:
		error(Ebadusefd);
	}

	s = c->aux;
	if(s == 0)
		error(Ebadusefd);

	switch(s->state){
	case Algwait:
		/* get algorithm */
		if(n >= sizeof(buf))
			Ebadarg;
		strncpy(buf, a, n);
		buf[n] = 0;
		s->blocklen = 1;
		s->diglen = 0;
		if(strcmp(buf, "md5") == 0){
			s->hf = md5;
			s->diglen = MD5dlen;
		} else if(strcmp(buf, "sha") == 0){
			s->hf = sha;
			s->diglen = SHAdlen;
		} else if(strcmp(buf, "descbc") == 0){
			s->encryptalg = DESCBC;
			s->blocklen = 8;
		} else if(strcmp(buf, "desecb") == 0){
			s->encryptalg = DESECB;
			s->blocklen = 8;
		} else
			error(Ebadarg);
		s->state = Fdwait;
		break;
	case Fdwait:
		/* get communications channel */
		s->c = buftochan(a, n);
		s->state = Secretinwait;
		break;
	case Secretinwait:
		/* get secret for incoming messages */
		setsecret(s, &s->in, a, n);
		s->state = Secretoutwait;
		break;
	case Secretoutwait:
		/* get secret for outgoing messages */
		setsecret(s, &s->out, a, n);
		if(s->blocklen != 1){
			s->max = (1<<15) - s->diglen;
			s->max -= s->max % s->blocklen;
			s->maxpad = (1<<14) - s->diglen;
			s->maxpad -= s->maxpad % s->blocklen;
		} else
			s->maxpad = s->max = (1<<15) - s->diglen;
		s->state = Established;
		break;
	case Established:
		p = a;
		for(e = p + n; p < e; p += m){
			m = e - p;
			if(m > s->max)
				m = s->max;
	
			b = allocb(m);
			if(waserror()){
				freeb(b);
				nexterror();
			}
			memmove(b->wp, p, m);
			poperror();
			b->wp += m;
	
			sslbwrite(c, b, offset);
		}
		break;
	default:
		error(Ebadusefd);
	}

	return n;
}

/*
 *  use SSL record format, add in count and digest or encrypt
 */
long
sslbwrite(Chan *c, Block *b, ulong offset)
{
	Dstate *s;
	Block *nb;
	int h, n, m, pad, rv;
	uchar *p;

	s = c->aux;
	if(s == 0 || s->state != Established)
		error(Ebadusefd);

	if(waserror()){
		qunlock(&s->out);
		if(b)
			freeb(b);
		dighangup(s);
		nexterror();
	}
	qlock(&s->out);

	rv = 0;
	while(b){
		m = n = BLEN(b);
		h = s->diglen + 2;

		/* trim to maximum block size */
		pad = 0;
		if(m > s->max){
			m = s->max;
		} else if(s->blocklen != 1){
			pad = m%s->blocklen;
			if(pad){
				if(m > s->maxpad){
					pad = 0;
					m = s->maxpad;
				} else {
					pad = s->blocklen - pad;
					h++;
				}
			}
		}

		rv += m;
		if(m != n){
			nb = allocb(m + h + pad);
			memmove(nb->wp + h, b->rp, m);
			nb->wp += m + h;
			b->rp += m;
		} else {
			/* add header space */
			nb = padblock(b, h);
			nb->rp -= h;
			b = 0;
		}
		m += s->diglen;

		/* SSL style count */
		if(pad){
			nb = padblock(nb, -pad);
			memset(nb->wp, 0, pad);
			nb->wp += pad;
			m += pad;

			p = nb->rp;
			p[0] = (m>>8);
			p[1] = m;
			p[2] = pad;
			offset = 3;
		} else {
			p = nb->rp;
			p[0] = (m>>8) | 0x80;
			p[1] = m;
			offset = 2;
		}

		if(s->encryptalg)
			nb = encryptb(s, nb, offset);
		else
			nb = digestb(s, nb, offset);

		(*devtab[s->c->type].bwrite)(s->c, nb, offset);

	}
	qunlock(&s->out);
	poperror();

	return rv;
}

/*
 *  make sure we have at least 'n' bytes in list 'l'
 */
static void
ensure(Dstate *s, Block **l, int n)
{
	int i, sofar;
	Block *b;

	b = *l;
	if(b){
		sofar = BLEN(b);
		l = &b->next;
	} else
		sofar = 0;

	while(sofar < n){
		b = (*devtab[s->c->type].bread)(s->c, Maxdmsg, 0);
		if(b == 0)
			error(Ehungup);
		i = BLEN(b);
		if(i <= 0){
			freeb(b);
			continue;
		}

		*l = b;
		l = &b->next;
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
		if(BLEN(b))
			break;
		*l = b->next;
		freeb(b);
	}
}

static void
setsecret(Dstate *s, OneWay *w, uchar *secret, int n)
{
	w->secret = smalloc(n);
	memmove(w->secret, secret, n);
	w->slen = n;
	w->mid = 0;

	switch(s->encryptalg){
	case DESECB:
		if(n < 8)
			error("secret too small");
		w->state = smalloc(sizeof(DESstate));
		setupDESstate(w->state, secret, 0);
		break;
	case DESCBC:
		if(n < 16)
			error("secret too small");
		w->state = smalloc(sizeof(DESstate));
		setupDESstate(w->state, secret, secret+8);
		break;
	}
}

static Block*
encryptb(Dstate *s, Block *b, int offset)
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
	}
	return b;
}

static Block*
decryptb(Dstate *s, Block *b)
{
	uchar *p, *ep, *tp, *ip, *eip;
	DESstate *ds;
	uchar tmp[8];

	switch(s->encryptalg){
	case DESECB:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + s->diglen; p < ep; p += 8)
			block_cipher(ds->expanded, p, 1);
		break;
	case DESCBC:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + s->diglen; p < ep;){
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
	}
	return b;
}

static Block*
digestb(Dstate *s, Block *b, int offset)
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
	n = w->mid++;
	*p++ = n>>24;
	*p++ = n>>16;
	*p++ = n>>8;
	*p = n;
	(*s->hf)(msgid, 4, b->rp + offset, &ss);

	return b;
}

static void
checkdigestb(Dstate *s, Block *b)
{
	uchar *p;
	DigestState ss;
	uchar msgid[4];
	int n, h;
	OneWay *w;
	uchar digest[128];

	w = &s->in;

	memset(&ss, 0, sizeof(ss));
	h = s->diglen;
	n = BLEN(b) - h;

	/* hash secret + message */
	(*s->hf)(w->secret, w->slen, 0, &ss);
	(*s->hf)(b->rp + h, n, 0, &ss);

	/* hash message id */
	p = msgid;
	n = w->mid++;
	*p++ = n>>24;
	*p++ = n>>16;
	*p++ = n>>8;
	*p = n;
	(*s->hf)(msgid, 4, digest, &ss);

	if(memcmp(digest, b->rp, s->diglen) != 0)
		error("bad digest");
}

/* get channel associated with an fd */
static Chan*
buftochan(char *a, long n)
{
	Chan *c;
	int fd;
	char buf[32];

	if(n >= sizeof buf)
		error(Egreg);
	memmove(buf, a, n);		/* so we can NUL-terminate */
	buf[n] = 0;
	fd = strtoul(buf, 0, 0);

	c = fdtochan(fd, -1, 0, 1);	/* error check and inc ref */
	return c;
}

/* hand up a digest connection */
static void
dighangup(Dstate *s)
{
	Block *b;

	qlock(&s->in);
	for(b = s->processed; b; b = s->processed){
		s->processed = b->next;
		freeb(b);
	}
	if(s->unprocessed){
		freeb(s->unprocessed);
		s->unprocessed = 0;
	}
	s->state = Closed;
	qunlock(&s->in);
}

/*
 *  crypt's interface to system, included here to override the
 *  library version
 */
void
handle_exception(int type, char *exception)
{
	if(type == CRITICAL)
		panic("kernel ssl: %s", exception);
	else
		print("kernel ssl: %s\n", exception);
}

void*
crypt_malloc(int size)
{
	void *x;

	x = smalloc(size);
	if(x == 0)
		handle_exception(CRITICAL, "out of memory");
	return x;
}

void
crypt_free(void *x)
{
	if(x == 0)
		handle_exception(CRITICAL, "freeing null pointer");
	free(x);
}

