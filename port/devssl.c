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
	DESCBC=		1,
};

typedef struct Dstate Dstate;
struct Dstate
{
	Chan	*c;		/* io channel */
	uchar	state;		/* state of connection */
	uchar	encryptalg;	/* encryption algorithm */
	ushort	blocking;	/* blocking length */

	ushort	diglen;		/* length of digest */
	DigestState *(*hf)(uchar*, ulong, uchar*, DigestState*);	/* hash func */

	int	max;		/* maximum unpadded data per msg */
	int	maxpad;	/* maximum padded data per msg */

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
	"digestclone",		{Qclone, 0},	0,	0600,
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

static void
setsecret(OneWay *w, uchar *secret, int n)
{
	w->secret = smalloc(n);
	memmove(w->secret, secret, n);
	w->slen = n;
	w->mid = 0;

	switch(s->encryptalg){
	case DESCBC:
		w->state = smalloc(sizeof(DESstate));
		setupDESstate(w->state, secret, 0);
		break;
	case DESCBC:
		w->state = smalloc(sizeof(DESstate));
		setupDESstate(w->state, secret, secret+8);
		break;
	}
}

long
sslwrite(Chan *c, char *a, long n, ulong offset)
{
	Dstate *s;
	Block *b;
	int m, sofar;
	char buf[32];

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
		} else if(strcmp(buf, "desebc") == 0){
			s->encryptalg = DESEBC;
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
		setsecret(&s->in, a, n);
		s->state = Secretoutwait;
		break;
	case Secretoutwait:
		/* get secret for outgoing messages */
		setsecret(&s->out, a, n);
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
		sofar = 0;
		do {
			m = n - sofar;
			if(m > s->max)
				m = s->max;
	
			b = allocb(m);
			if(waserror()){
				freeb(b);
				nexterror();
			}
			memmove(b->wp, a+sofar, m);
			poperror();
			b->wp += m;
	
			sslbwrite(c, b, offset);
	
			sofar += m;
		} while(sofar < n);
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

		/* padded blocks are shorter than unpadded ones (strange) */
		pad = 0;
		if(m > s->max){
			m = s->max;
		} else if(s->blocklen != 1){
			pad = m%s->blocklen;
			if(pad){
				pad = s->blocklen - pad;
				if(m > s->maxpad){
					pad = 0;
					m = s->maxpad;
				}
			}
		}

		rv += m;
		if(m != n){
			nb = allocb(m + h + pad);
			memmove(nb->wp + h, m, b->rp);
			nb->wp += m + h;
			b->rp += m;
		} else {
			/* add header */
			nb = padblock(b, h);
			nb->rp -= h;

			/* add pad */
			if(pad)
				nb = padblock(nb, -pad);
			b = 0;
		}
		m += s->diglen;

		/* SSL style count */
		if(pad){
			memset(nb->wp, 0, pad);
			m += pad;
			nb->wp += pad;
		} else
			m |= 0x8000;
		np->rp[0] = (m>>8);
		np->rp[1] = m;

		if(encryptalg)
			encryptb(s, nb);
		else
			digestb(s, nb);

		(*devtab[s->c->type].bwrite)(s->c, nb, offset);

	}
	qunlock(&s->out);
	poperror();

	return rv;
}

Block*
sslbread(Chan *c, long n, ulong offset)
{
	Block *bp, **l;
	uchar count[2];
	int len;
	int pad;

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

	if(s->processed == 0){
	
		/* read in the whole message */
		s->processed = s->unprocessed;
		s->unprocessed =- 0;
		ensure(s, &s->processed, 2);
		consume(&s->processed, count, 2);
		if(count[0] & 0x80){
			len = ((count[0] & 0x7f)<<8) | count[1];
			pad = 0;
		} else {
			len = ((count[0] & 0x3f)<<8) | count[1];
			ensure(s, &s->processed, 1);
			consume(&s->processed, count, 1);
			pad = count[0];
		}
		ensure(s, &s->processed, len);

		/* put remainder on unprocessed */
		i = 0;
		for(b = s->processed; b; b = b->next){
			i = BLEN(b);
			if(i >= len)
				break;
			(*s->func)(b->rp, i, 0, &ss);
			len -= i;
		}
		if(b == 0)
			panic("digestbread");
		if(i > len){
			i -= len;
			s->unprocessed = allocb(i);
			memmove(s->unprocessed->wp, b->rp+len, i);
			s->unprocessed->wp += i;
			b->wp -= i;
		}
			
		if(s->encrypalg)
			decryptb(s, len);
		else
			checkdigestb(s, len);

		if(pad){
			for(b = s->processed; b; b = b->next){
	}

	b = s->processed;
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

Block*
decryptb(Dstate *s, Block *b, int len)
{
	ulong n, h;
	uchar *p, *ep;
	DESstate *ds;

	h = s->diglen + 2;

	switch(s->encryptalg){
	case DESEBC:
		ds = s->in.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + h; p < ep; p += 8)
			block_cipher(ds->expanded, p, 1);
		break;
	case DESCBC:
		ds = s->in.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + h; p < ep; p += 8)
			bCBCDecrypt(p, ds->ivec, ds->expanded, 8);
		break;
	}
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

static Block*
digestbread(Dstate *s, long n)
{
	Block *b;
	int i, m, len;
	uchar *p;
	uchar *dp;
	uchar digestin[32];
	uchar digest[32];
	DigestState ss;

	memset(&ss, 0, sizeof(ss));

	ensure(s, &s->unprocessed, s->diglen);
		len = 0;
		for(i = 0; i < 4; i++){
			consume(&s->unprocessed, digestin+i, 1);
			m = digestin[i];
			if((m & 0x80) == 0)
				break;
			len = (len<<7) | (m & 0x7f);
		}
	
		/* digest count */
		p = &digestin[s->diglen];
		(*s->func)(p, i, 0, &ss);
		ensure(s, &s->unprocessed, s->diglen);

		/* get message */
		s->processed = s->unprocessed;
		s->unprocessed = 0;
		ensure(s, &s->processed, len);

		/* digest message */
		i = 0;
		for(b = s->processed; b; b = b->next){
			i = BLEN(b);
			if(i >= len)
				break;
			(*s->func)(b->rp, i, 0, &ss);
			len -= i;
		}
		if(b == 0)
			panic("digestbread");
		if(i > len){
			i -= len;
			s->unprocessed = allocb(i);
			memmove(s->unprocessed->wp, b->rp+len, i);
			s->unprocessed->wp += i;
			b->wp -= i;
		}
		(*s->func)(b->rp, len, 0, &ss);

		/* digest secret & message id */
		p = s->in.secret;
		m = s->in.mid++;
		*p++ = m>>24;
		*p++ = m>>16;
		*p++ = m>>8;
		*p = m;
		(*s->func)(s->in.secret, s->in.slen, digest, &ss);

		if(memcmp(digest, digestin, s->diglen) != 0)
			error("bad digest");
	}

	b = s->processed;
	if(BLEN(b) > n){
		b = allocb(n);
		memmove(b->wp, s->processed->rp, n);
		b->wp += n;
		s->processed->rp += n;
	} else 
		s->processed = b->next;

	return b;
}

void
digestb(Dstate *s, Block *b)
{
	Block *nb;
	uchar *p;
	DigestState ss;
	uchar msgid[4];
	ulong n, h;
	OneWay *w;

	w = &s->out;

	memset(&ss, 0, sizeof(ss));
	h = s->diglen + 2;
	n = BLEN(b) - h;

	/* hash secret + message */
	(*s->hf)(w->secret, w->slen, 0, &ss);
	(*s->hf)(nb->rp + h, n, 0, &ss);

	/* hash message id */
	p = msgid;
	n = w->mid++;
	*p++ = n>>24;
	*p++ = n>>16;
	*p++ = n>>8;
	*p = n;
	(*s->func)(msgid, 4, nb->rp + 2, &ss);
}

long
encryptb(Dstate *s, Block *b)
{
	ulong n, h;
	int j;
	uchar *p, *ep, *ip;
	DESstate *ds;

	h = s->diglen + 2;

	switch(s->encryptalg){
	case DESEBC:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + h; p < ep; p += 8)
			block_cipher(ds->expanded, p, 0);
		break;
	case DESCBC:
		ds = s->out.state;
		ep = b->rp + BLEN(b);
		for(p = b->rp + h; p < ep; p += 8)
			bCBCEncrypt(p, ds->ivec, ds->expanded, 8);
		break;
	}
	
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

