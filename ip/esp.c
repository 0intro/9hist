#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

#include	"libcrypt.h"

typedef struct Esphdr Esphdr;
typedef struct Esptail Esptail;
typedef struct Userhdr Userhdr;
typedef struct Esppriv Esppriv;
typedef struct Espcb Espcb;
typedef struct Algorithm Algorithm;

#define DPRINT if(0)print

enum
{
	IP_ESPPROTO	= 50,
	EsphdrSize	= 28,	// includes IP header
	IphdrSize	= 20,	// options have been striped
	EsptailSize	= 2,	// does not include pad or auth data
	UserhdrSize	= 4,	// user visable header size - if enabled
};

struct Esphdr
{
	/* ip header */
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* Identification */
	uchar	frag[2];	/* Fragment information */
	uchar	Unused;	
	uchar	espproto;	/* Protocol */
	uchar	espplen[2];	/* Header plus data length */
	uchar	espsrc[4];	/* Ip source */
	uchar	espdst[4];	/* Ip destination */

	/* esp header */
	uchar	espspi[4];	/* Security parameter index */
	uchar	espseq[4];	/* Sequence number */
};

struct Esptail
{
	uchar	pad;
	uchar	nexthdr;
};

/* header as seen by the user */
struct Userhdr
{
	uchar	nexthdr;	// next protocol
	uchar	unused[3];
};

struct Esppriv
{
	ulong	in;
	ulong	inerrors;
};

/*
 *  protocol specific part of Conv
 */
struct Espcb
{
	int	incoming;
	int	header;		// user user level header
	ulong	spi;
	ulong	seq;		// last seq sent
	ulong	window;		// for replay attacks
	char	*espalg;
	void	*espstate;	// other state for esp
	int	espivlen;	// in bytes
	int	espblklen;
	int	(*cipher)(Espcb*, uchar *buf, int len);
	char	*ahalg;
	void	*ahstate;	// other state for esp
	int	ahlen;		// auth data length in bytes
	int	ahblklen;
	int	(*auth)(Espcb*, uchar *buf, int len, uchar *hash);
};

struct Algorithm
{
	char 	*name;
	int	keylen;		// in bits
	void	(*init)(Espcb*, char* name, uchar *key, int keylen);
};

static	Conv* convlookup(Proto *esp, ulong spi);
static	char *setalg(Espcb *ecb, char **f, int n, Algorithm *alg);
static	void nullespinit(Espcb*, char*, uchar *key, int keylen);
static	void nullahinit(Espcb*, char*, uchar *key, int keylen);
static	void desespinit(Espcb *ecb, char *name, uchar *k, int n);

static Algorithm espalg[] =
{
	"null",			0,	nullespinit,
	"des_56_cbc",		64,	desespinit,
	nil,			0,	nil,
};

static Algorithm ahalg[] =
{
	"null",			0,	nullahinit,
	nil,			0,	nil,
};

static char*
espconnect(Conv *c, char **argv, int argc)
{
	char *p, *pp;
	char *e = nil;
	ulong spi;
	Espcb *ecb = (Espcb*)c->ptcl;

	qlock(c);
	switch(argc) {
	default:
		e = "bad args to connect";
		break;
	case 2:
		p = strchr(argv[1], '!');
		if(p == nil){
			e = "malformed address";
			break;
		}
		*p++ = 0;
		parseip(c->raddr, argv[1]);
		findlocalip(c->p->f, c->laddr, c->raddr);
		ecb->incoming = 0;
		ecb->seq = 0;
		if(strcmp(p, "*") == 0) {
			qlock(c->p);
			for(;;) {
				spi = nrand(1<<16) + 256;
				if(convlookup(c->p, spi) == nil)
					break;
			}
			qunlock(c->p);
			ecb->spi = spi;
			ecb->incoming = 1;
			qhangup(c->wq, nil);
		} else {
			spi = strtoul(p, &pp, 10);
			if(pp == p) {
				e = "malformed address";
				break;
			}
			ecb->spi = spi;
			qhangup(c->rq, nil);
		}
		nullespinit(ecb, "null", nil, 0);
		nullahinit(ecb, "null", nil, 0);
	}
	qunlock(c);
	Fsconnected(c, e);

	return e;
}


static int
espstate(Conv *c, char *state, int n)
{
	return snprint(state, n, "%s", c->inuse?"Open\n":"Closed\n");
}

static void
espcreate(Conv *c)
{
	c->rq = qopen(64*1024, 1, 0, 0);
	c->wq = qopen(64*1024, 0, 0, 0);
}

static void
espclose(Conv *c)
{
	Espcb *ecb;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);

	ecb = (Espcb*)c->ptcl;
	free(ecb->espstate);
	free(ecb->ahstate);
	memset(ecb, 0, sizeof(Espcb));
	
	qunlock(c);
}

void
espkick(Conv *c, int)
{
	Esphdr *eh;
	Esptail *et;
	Userhdr *uh;
	Espcb *ecb;
	Block *bp;
	int nexthdr;
	int payload;
	int pad;
	int align;
	uchar *auth;

	bp = qget(c->wq);
	if(bp == nil)
		return;

	qlock(c);
	ecb = c->ptcl;

	if(ecb->header) {
		/* make sure the message has a User header */
		bp = pullupblock(bp, UserhdrSize);
		if(bp == nil) {
			qunlock(c);
			return;
		}
		uh = (Userhdr*)bp->rp;
		nexthdr = uh->nexthdr;
		bp->rp += UserhdrSize;
	} else {
		nexthdr = 0;  // what should this be?
	}

	payload = BLEN(bp) + ecb->espivlen;

	/* Make space to fit ip header */
	bp = padblock(bp, EsphdrSize + ecb->espivlen);

	align = 4;
	if(ecb->espblklen > align)
		align = ecb->espblklen;
	if(align % ecb->ahblklen != 0)
		panic("espkick: ahblklen is important after all");
	pad = (align-1) - (payload + EsptailSize-1)%align;

	/*
	 * Make space for tail
	 * this is done by calling padblock with a negative size
	 * Padblock does not change bp->wp!
	 */
	bp = padblock(bp, -(pad+EsptailSize+ecb->ahlen));
	bp->wp += pad+EsptailSize+ecb->ahlen;

	eh = (Esphdr *)(bp->rp);
	et = (Esptail*)(bp->rp + EsphdrSize + payload + pad);

	// fill in tail
	et->pad = pad;
	et->nexthdr = nexthdr;

	ecb->cipher(ecb, bp->rp+EsphdrSize, payload+pad+EsptailSize);
	auth = bp->rp + EsphdrSize + payload + pad + EsptailSize;
	ecb->auth(ecb, bp->rp+IphdrSize, (EsphdrSize-IphdrSize)+payload+pad+EsptailSize, auth);

	// fill in head
	hnputl(eh->espspi, ecb->spi);
	hnputl(eh->espseq, ++ecb->seq);
	v6tov4(eh->espsrc, c->laddr);
	v6tov4(eh->espdst, c->raddr);
	eh->espproto = IP_ESPPROTO;
	eh->frag[0] = 0;
	eh->frag[1] = 0;

	qunlock(c);
//print("esp: pass down: %uld\n", BLEN(bp));
	ipoput(c->p->f, bp, 0, c->ttl);
}

void
espiput(Proto *esp, uchar*, Block *bp)
{
	Esphdr *eh;
	Esptail *et;
	Userhdr *uh;
	Conv *c;
	Espcb *ecb;
	uchar raddr[IPaddrlen], laddr[IPaddrlen];
	Fs *f;
	uchar *auth;
	ulong spi;
	int payload, nexthdr;

	f = esp->f;

	bp = pullupblock(bp, EsphdrSize+EsptailSize);
	if(bp == nil) {
		netlog(f, Logesp, "esp: short packet\n");
		return;
	}

	eh = (Esphdr*)(bp->rp);
	spi = nhgetl(eh->espspi);
	v4tov6(raddr, eh->espsrc);
	v4tov6(laddr, eh->espdst);

	qlock(esp);
	/* Look for a conversation structure for this port */
	c = convlookup(esp, spi);
	if(c == nil) {
		qunlock(esp);
		netlog(f, Logesp, "esp: no conv %I -> %I!%d\n", raddr,
			laddr, spi);
		icmpnoconv(f, bp);
		freeblist(bp);
		return;
	}

	qlock(c);
	qunlock(esp);

	ecb = c->ptcl;
	// too hard to do decryption/authentication on block lists
	if(bp->next)
		bp = concatblock(bp);

	if(BLEN(bp) < EsphdrSize + ecb->espivlen + EsptailSize + ecb->ahlen) {
		qunlock(esp);
		netlog(f, Logesp, "esp: short block %I -> %I!%d\n", raddr,
			laddr, spi);
		freeb(bp);
		return;
	}

	eh = (Esphdr*)(bp->rp);
	auth = bp->wp - ecb->ahlen;
	if(!ecb->auth(ecb, eh->espspi, auth-eh->espspi, auth)) {
		qunlock(esp);
		netlog(f, Logesp, "esp: bad auth %I -> %I!%d\n", raddr,
			laddr, spi);
		freeb(bp);
		return;
	}

	payload = BLEN(bp)-EsphdrSize-ecb->ahlen;
	if(payload<=0 || payload%4 != 0 || payload%ecb->espblklen!=0) {
		qunlock(esp);
		netlog(f, Logesp, "esp: bad length %I -> %I!%d payload=%d BLEN=%d\n", raddr,
			laddr, spi, payload, BLEN(bp));
		freeb(bp);
		return;
	}
	ecb->cipher(ecb, bp->rp+EsphdrSize, payload);

	payload -= EsptailSize;
	et = (Esptail*)(bp->rp + EsphdrSize + payload);
	payload -= et->pad + ecb->espivlen;
	nexthdr = et->nexthdr;
	if(payload <= 0) {
		qunlock(esp);
		netlog(f, Logesp, "esp: short packet after decrypt %I -> %I!%d\n", raddr,
			laddr, spi);
		freeb(bp);
		return;
	}

	// trim packet
	bp->rp += EsphdrSize + ecb->espivlen;
	bp->wp = bp->rp + payload;
	if(ecb->header) {
		// assume UserhdrSize < EsphdrSize
		bp->rp -= UserhdrSize;
		uh = (Userhdr*)bp->rp;
		memset(uh, 0, UserhdrSize);
		uh->nexthdr = nexthdr;
	}

	if(qfull(c->rq)){
		netlog(f, Logesp, "esp: qfull %I -> %I.%uld\n", raddr,
			laddr, spi);
		freeblist(bp);
	}else {
//print("esp: pass up: %uld\n", BLEN(bp));
		qpass(c->rq, bp);
	}

	qunlock(c);
}

char*
espctl(Conv *c, char **f, int n)
{
	Espcb *ecb = c->ptcl;
	char *e = nil;

	qlock(c);
	if(strcmp(f[0], "esp") == 0)
		e = setalg(ecb, f, n, espalg);
	else if(strcmp(f[0], "ah") == 0)
		e = setalg(ecb, f, n, ahalg);
	else if(strcmp(f[0], "header") == 0)
		ecb->header = 1;
	else if(strcmp(f[0], "noheader") == 0)
		ecb->header = 0;
	else
		e = "unknown control request";
	qunlock(c);
	return e;
}

void
espadvise(Proto *esp, Block *bp, char *msg)
{
	Esphdr *h;
	Conv *c;
	ulong spi;

	h = (Esphdr*)(bp->rp);

	spi = nhgets(h->espspi);
	qlock(esp);
	c = convlookup(esp, spi);
	if(c != nil) {
		qhangup(c->rq, msg);
		qhangup(c->wq, msg);
	}
	qunlock(esp);
	freeblist(bp);
}

int
espstats(Proto *esp, char *buf, int len)
{
	Esppriv *upriv;

	upriv = esp->priv;
	return snprint(buf, len, "%lud %lud\n",
		upriv->in,
		upriv->inerrors);
}

static int
esplocal(Conv *c, char *buf, int len)
{
	Espcb *ecb = c->ptcl;
	int n;

	qlock(c);
	if(ecb->incoming)
		n = snprint(buf, len, "%I!%uld\n", c->laddr, ecb->spi);
	else
		n = snprint(buf, len, "%I\n", c->laddr);
	qunlock(c);
	return n;
}

static int
espremote(Conv *c, char *buf, int len)
{
	Espcb *ecb = c->ptcl;
	int n;

	qlock(c);
	if(ecb->incoming)
		n = snprint(buf, len, "%I\n", c->raddr);
	else
		n = snprint(buf, len, "%I!%uld\n", c->raddr, ecb->spi);
	qunlock(c);
	return n;
}

// should have esp locked
static	Conv*
convlookup(Proto *esp, ulong spi)
{
	Conv *c, **p;
	Espcb *ecb;

	for(p=esp->conv; *p; p++){
		c = *p;
		ecb = c->ptcl;
		if(ecb->incoming && ecb->spi == spi)
			return c;
	}
}

static char *
setalg(Espcb *ecb, char **f, int n, Algorithm *alg)
{
	uchar *key;
	int i, nbyte, nchar;
	int c;

	if(n < 2)
		return "bad format";
	for(; alg->name; alg++)
		if(strcmp(f[1], alg->name) == 0)
			break;
	if(alg->name == nil)
		return "unknown algorithm";

	if(n != 3)
		return "bad format";
	nbyte = (alg->keylen + 7) >> 3;
	nchar = strlen(f[2]);
	for(i=0; i<nchar; i++) {
		c = f[2][i];
		if(c >= '0' && c <= '9')
			f[2][i] -= '0';
		else if(c >= 'a' && c <= 'f')
			f[2][i] -= 'a'-10;
		else if(c >= 'A' && c <= 'F')
			f[2][i] -= 'A'-10;
		else
			return "bad character in key";
	}
	key = smalloc(nbyte);
	for(i=0; i<nchar && i*2<nbyte; i++) {
		c = f[2][nchar-i-1];
		if(i&1)
			c <<= 4;
		key[i>>1] |= c;
	}

	alg->init(ecb, alg->name, key, alg->keylen);
	free(key);
	return nil;
}

static int
nullcipher(Espcb*, uchar*, int)
{
	return 1;
}

static void
nullespinit(Espcb *ecb, char *name, uchar*, int)
{
	ecb->espalg = name;
	ecb->espblklen = 1;
	ecb->espivlen = 0;
	ecb->cipher = nullcipher;
}

static int
nullauth(Espcb*, uchar*, int, uchar*)
{
	return 1;
}

static void
nullahinit(Espcb *ecb, char *name, uchar*, int)
{
	ecb->ahalg = name;
	ecb->ahblklen = 1;
	ecb->ahlen = 0;
	ecb->auth = nullauth;
}

static int
descipher(Espcb *ecb, uchar *p, int n)
{
	uchar tmp[8];
	uchar *pp, *tp, *ip, *eip, *ep;
	DESstate *ds = ecb->espstate;

	ep = p + n;
	if(ecb->incoming) {
		memmove(ds->ivec, p, 8);
		p += 8;
		while(p < ep){
			memmove(tmp, p, 8);
			block_cipher(ds->expanded, p, 1);
			tp = tmp;
			ip = ds->ivec;
			for(eip = ip+8; ip < eip; ){
				*p++ ^= *ip;
				*ip++ = *tp++;
			}
		}
	} else {
		memmove(p, ds->ivec, 8);
		for(p += 8; p < ep; p += 8){
			pp = p;
			ip = ds->ivec;
			for(eip = ip+8; ip < eip; )
				*pp++ ^= *ip++;
			block_cipher(ds->expanded, p, 0);
			memmove(ds->ivec, p, 8);
		}
	}
	return 1;
}
	
static void
desespinit(Espcb *ecb, char *name, uchar *k, int n)
{
	uchar key[8];
	uchar ivec[8];
	int i;
	
	// bits to bytes
	n = (n+7)>>3;
	if(n > 8)
		n = 8;
	memset(key, 0, sizeof(key));
	memmove(key, k, n);
	for(i=0; i<8; i++)
		ivec[i] = nrand(256);
	ecb->espalg = name;
	ecb->espblklen = 8;
	ecb->espivlen = 8;
	ecb->cipher = descipher;
	ecb->espstate = smalloc(sizeof(DESstate));
	setupDESstate(ecb->espstate, key, ivec);
}
	
void
espinit(Fs *fs)
{
	Proto *esp;

	esp = smalloc(sizeof(Proto));
	esp->priv = smalloc(sizeof(Esppriv));
	esp->name = "esp";
	esp->kick = espkick;
	esp->connect = espconnect;
	esp->announce = nil;
	esp->ctl = espctl;
	esp->state = espstate;
	esp->create = espcreate;
	esp->close = espclose;
	esp->rcv = espiput;
	esp->advise = espadvise;
	esp->stats = espstats;
	esp->local = esplocal;
	esp->remote = espremote;
	esp->ipproto = IP_ESPPROTO;
	esp->nc = Nchans;
	esp->ptclsize = sizeof(Espcb);

	Fsproto(fs, esp);
}
