#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

typedef struct Pnp Pnp;
typedef struct Card Card;

struct Pnp
{
	Lock;
	int		rddata;
	int		debug;
	Card		*cards;
};

struct Card
{
	int		csn;
	ulong	id1;
	ulong	id2;
	int		ncfg;
	Card*	next;
};

static Pnp	pnp;

#define	DPRINT	if(pnp.debug) print
#define	XPRINT	if(1) print

enum {
	Address = 0x279,
	WriteData = 0xa79,

	Qtopdir = 0,

	Qpnpdir,
	Qpnpctl,
	Qcsnctl,
	Qcsnraw,

	Qpcidir,
	Qpcictl,
	Qpciraw,
};

#define TYPE(q)		((ulong)(q).path & 0x0F)
#define CSN(q)		(((ulong)(q).path>>4) & 0xFF)
#define QID(c, t)	(((c)<<4)|(t))

static Dirtab topdir[] = {
	".",	{ Qtopdir, 0, QTDIR },	0,	0555,
	"pnp",	{ Qpnpdir, 0, QTDIR },	0,	0555,
	"pci",	{ Qpcidir, 0, QTDIR },	0,	0555,
};

static Dirtab pnpdir[] = {
	".",	{ Qpnpdir, 0, QTDIR },	0,	0555,
	"ctl",	{ Qpnpctl, 0, 0 },	0,	0666,
};

extern Dev pnpdevtab;

static char key[32] =
{
	0x6A, 0xB5, 0xDA, 0xED, 0xF6, 0xFB, 0x7D, 0xBE,
	0xDF, 0x6F, 0x37, 0x1B, 0x0D, 0x86, 0xC3, 0x61,
	0xB0, 0x58, 0x2C, 0x16, 0x8B, 0x45, 0xA2, 0xD1,
	0xE8, 0x74, 0x3A, 0x9D, 0xCE, 0xE7, 0x73, 0x39,
};

static void
cmd(int reg, int val)
{
	outb(Address, reg);
	outb(WriteData, val);
}

/* Send initiation key, putting each card in Sleep state */
static void
initiation(void)
{
	int i;

	/* ensure each card's LFSR is reset */
	outb(Address, 0x00);
	outb(Address, 0x00);

	/* send initiation key */
	for (i = 0; i < 32; i++)
		outb(Address, key[i]);
}

/* isolation protocol... */
static int
readbit(int rddata)
{
	int r1, r2;

	r1 = inb(rddata);
	r2 = inb(rddata);
	microdelay(250);
	return (r1 == 0x55) && (r2 == 0xaa);
}

static int
isolate(int rddata, ulong *id1, ulong *id2)
{
	int i, csum, bit;
	uchar *p, id[9];

	outb(Address, 0x01);	/* point to serial isolation register */
	delay(1);
	csum = 0x6a;
	for (i = 0; i < 64; i++) {
		bit = readbit(rddata);
		csum = (csum>>1) | (((csum&1) ^ ((csum>>1)&1) ^ bit)<<7);
		p = &id[i>>3];
		*p = (*p>>1) | (bit<<7);
	}
	for (; i < 72; i++) {
		p = &id[i>>3];
		*p = (*p>>1) | (readbit(rddata)<<7);
	}
	*id1 = (id[3]<<24)|(id[2]<<16)|(id[1]<<8)|id[0];
	*id2 = (id[7]<<24)|(id[6]<<16)|(id[5]<<8)|id[4];
	if (*id1 == 0)
		return 0;
	if (id[8] != csum)
		DPRINT("pnp: bad checksum id1 %lux id2 %lux csum %x != %x\n", *id1, *id2, csum, id[8]); /**/
	return id[8] == csum;
}

static int
getresbyte(int rddata)
{
	int tries = 0;

	outb(Address, 0x05);
	while ((inb(rddata) & 1) == 0)
		if (tries++ > 1000000)
			error("pnp: timeout waiting for resource data\n");
	outb(Address, 0x04);
	return inb(rddata);
}

static char *
serial(ulong id1, ulong id2)
{
	int i1, i2, i3;
	ulong x;
	static char buf[20];

	i1 = (id1>>2)&31;
	i2 = ((id1<<3)&24)+((id1>>13)&7);
	i3 = (id1>>8)&31;
	x = (id1>>8)&0xff00|(id1>>24)&0x00ff;
	if (i1 > 0 && i1 < 27 && i2 > 0 && i2 < 27 && i3 > 0 && i3 < 27 && (id1 & (1<<7)) == 0)
		snprint(buf, sizeof(buf), "%c%c%c%.4lux.%lux", 'A'+i1-1, 'A'+i2-1, 'A'+i3-1, x, id2);
	else
		snprint(buf, sizeof(buf), "%.4lux%.4lux.%lux", (id1<<8)&0xff00|(id1>>8)&0x00ff, x, id2);
	return buf;
}

/* called with pnp locked */
static Card *
findcsn(int csn, int create)
{
	Card *c, *nc, **l;

	l = &pnp.cards;
	for(c = *l; c != nil; c = *l) {
		if(c->csn == csn)
			return c;
		if(c->csn > csn)
			break;
		l = &c->next;
	}
	if(!create)
		return nil;
	*l = nc = malloc(sizeof(Card));
	nc->next = c;
	nc->csn = csn;
	return nc;
}

static int
newcsn(void)
{
	int csn;
	Card *c;

	csn = 1;
	for(c = pnp.cards; c != nil; c = c->next) {
		if(c->csn > csn)
			break;
		csn = c->csn+1;
	}
	return csn;
}

static int
pnpncfg(int rddata)
{
	int i, n, x, ncfg, n1, n2;

	ncfg = 0;
	for (;;) {
		x = getresbyte(rddata);
		if((x & 0x80) == 0) {
			n = (x&7)+1;
			for(i = 1; i < n; i++)
				getresbyte(rddata);
		}
		else {
			n1 = getresbyte(rddata);
			n2 = getresbyte(rddata);
			n = (n2<<8)|n1 + 3;
			for (i = 3; i < n; i++)
				getresbyte(rddata);
		}
		ncfg += n;
		if((x>>3) == 0x0f)
			break;
	}
	return ncfg;
}

/* look for cards, and assign them CSNs */
static int
pnpscan(int rddata)
{
	Card *c;
	int csn, ok;
	ulong id1, id2;

	ilock(&pnp);
	pnp.rddata = rddata;
	initiation();
	cmd(0x02, 0x04+0x01);		/* reset CSN on all cards and reset logical devices */
	delay(1);					/* delay after resetting cards */

	cmd(0x03, 0);				/* Wake all cards with a CSN of 0 */
	cmd(0x00, rddata>>2);		/* Set the READ_DATA port on all cards */
	while(isolate(rddata, &id1, &id2)) {
		for(c = pnp.cards; c != nil; c = c->next)
			if(c->id1 == id1 && c->id2 == id2)
				break;
		if(c == nil) {
			csn = newcsn();
			c = findcsn(csn, 1);
			c->id1 = id1;
			c->id2 = id2;
		}
		cmd(0x06, c->csn);		/* set the card's csn */
		print("pnp%d: %s\n", c->csn, serial(id1, id2));
		c->ncfg = pnpncfg(rddata);
		cmd(0x03, 0);		/* Wake all cards with a CSN of 0, putting this card to sleep */
	}
	cmd(0x02, 0x02);			/* return cards to Wait for Key state */
	ok = (pnp.cards != 0);
	iunlock(&pnp);
	return ok;
}

static void
pnpreset(void)
{
	Card *c;
	ulong id1, id2;
	int csn, i1, i2, i3, x;
	char *s, *p, buf[20];
	ISAConf isa;

	memset(&isa, 0, sizeof(ISAConf));
	pnp.rddata = -1;
	if (isaconfig("pnp", 0, &isa) == 0)
		return;
	if(isa.port < 0x203 || isa.port > 0x3ff)
		return;
	for(csn = 1; csn < 256; csn++) {
		sprint(buf, "pnp%d", csn);
		s = getconf(buf);
		if(s == 0)
			continue;
		if(strlen(s) < 8 || s[7] != '.' || s[0] < 'A' || s[0] > 'Z' || s[1] < 'A' || s[1] > 'Z' || s[2] < 'A' || s[2] > 'Z') {
bad:
			print("pnp%d: bad conf string %s\n", csn, s);
			continue;	
		}
		i1 = s[0]-'A'+1;
		i2 = s[1]-'A'+1;
		i3 = s[2]-'A'+1;
		x = strtoul(&s[3], 0, 16);
		id1 = (i1<<2)|((i2>>3)&3)|((i2&7)<<13)|(i3<<8)|((x&0xff)<<24)|((x&0xff00)<<8);
		id2 = strtoul(&s[8], &p, 16);
		if(*p != ' ' && *p != '\0')
			goto bad;
		c = findcsn(csn, 1);
		c->id1 = id1;
		c->id2 = id2;
	}
	pnpscan(isa.port);
}

static int
pnpgen1(Chan *c, int t, int csn, Card *cp, Dir *dp)
{
	Qid q;
	char buf[20];

	switch(t) {
	case Qcsnctl:
		q = (Qid){QID(csn, Qcsnctl), 0, 0};
		sprint(buf, "csn%dctl", csn);
		devdir(c, q, buf, 0, eve, 0664, dp);
		return 1;
	case Qcsnraw:
		q = (Qid){QID(csn, Qcsnraw), 0, 0};
		sprint(buf, "csn%draw", csn);
		devdir(c, q, buf, cp->ncfg, eve, 0444, dp);
		return 1;
	}
	return -1;
}

static int
pnpgen(Chan *c, char *, Dirtab*, int, int s, Dir *dp)
{
	Qid q;
	int csn;
	Card *cp;
	char name[KNAMELEN];

DPRINT("pnpgen s %d offset %uld qid %lux\n", s, (ulong)c->offset, (ulong)c->qid.path);
	switch(TYPE(c->qid)){
	case Qtopdir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, Qtopdir), 0, QTDIR};
			snprint(name, KNAMELEN, "#%C", pnpdevtab.dc);
			devdir(c, q, name, 0, eve, 0555, dp);
			return 1;
		}
		return devgen(c, nil, topdir, nelem(topdir), s, dp);
	case Qpnpdir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, Qtopdir), 0, QTDIR};
			snprint(name, KNAMELEN, "#%C", pnpdevtab.dc);
			devdir(c, q, name, 0, eve, 0555, dp);
			return 1;
		}
DPRINT("Qpnpdir s %d\n", s);
		if(s < nelem(pnpdir))
			return devgen(c, nil, pnpdir, nelem(pnpdir), s, dp);
		s -= nelem(pnpdir);
DPRINT("Qpnpdir s now %d\n", s);
		ilock(&pnp);
		cp = pnp.cards;
		while(s >= 2 && cp != nil) {
			s -= 2;
			cp = cp->next;
		}
		iunlock(&pnp);
DPRINT("Qpnpdir s now %d, cp %p\n", s, cp);
		if(cp == nil)
			return -1;
		return pnpgen1(c, s+Qcsnctl, cp->csn, cp, dp);
	case Qpnpctl:
		return devgen(c, nil, pnpdir, nelem(pnpdir), s, dp);
	case Qcsnctl:
	case Qcsnraw:
		csn = CSN(c->qid);
		ilock(&pnp);
		cp = findcsn(csn, 0);
		iunlock(&pnp);
		if(cp == nil)
			return -1;
		return pnpgen1(c, TYPE(c->qid), csn, cp, dp);
	default:
		break;
	}
	return -1;
}

static Chan*
pnpattach(char *spec)
{
	return devattach(pnpdevtab.dc, spec);
}

Walkqid*
pnpwalk(Chan* c, Chan *nc, char** name, int nname)
{
	return devwalk(c, nc, name, nname, (Dirtab *)0, 0, pnpgen);
}

static int
pnpstat(Chan* c, uchar* dp, int n)
{
	return devstat(c, dp, n, (Dirtab *)0, 0L, pnpgen);
}

static Chan*
pnpopen(Chan *c, int omode)
{
	c = devopen(c, omode, (Dirtab*)0, 0, pnpgen);
	switch(TYPE(c->qid)){
	default:
		break;
	}
	return c;
}

static void
pnpclose(Chan*)
{
}

static long
pnpread(Chan *c, void *va, long n, vlong offset)
{
	int csn, i;
	Card *cp;
	char *a = va, buf[20];

	switch(TYPE(c->qid)){
	case Qtopdir:
	case Qpnpdir:
	case Qpcidir:
		return devdirread(c, a, n, (Dirtab *)0, 0L, pnpgen);
	case Qpnpctl:
		if(pnp.rddata > 0)
			sprint(buf, "enabled 0x%x\n", pnp.rddata);
		else
			sprint(buf, "disabled\n");
		return readstr(offset, a, n, buf);
	case Qcsnraw:
		csn = CSN(c->qid);
		ilock(&pnp);
		cp = findcsn(csn, 0);
		iunlock(&pnp);
		if(cp == nil)
			error(Egreg);
		if(offset+n > cp->ncfg)
			n = cp->ncfg - offset;
		ilock(&pnp);
		initiation();
		cmd(0x03, csn);				/* Wake up the card */
		for (i = 0; i < offset+9; i++)		/* 9 == skip serial + csum */
			getresbyte(pnp.rddata);
		for (i = 0; i < n; i++)
			a[i] = getresbyte(pnp.rddata);
		cmd(0x03, 0);					/* Wake all cards with a CSN of 0, putting this card to sleep */
		cmd(0x02, 0x02);				/* return cards to Wait for Key state */
		iunlock(&pnp);
		break;
	case Qcsnctl:
		csn = CSN(c->qid);
		ilock(&pnp);
		cp = findcsn(csn, 0);
		iunlock(&pnp);
		if(cp == nil)
			error(Egreg);
		sprint(buf, "%s\n", serial(cp->id1, cp->id2));
		return readstr(offset, a, n, buf);
	default:
		error(Egreg);
	}
	return n;
}

static long
pnpwrite(Chan *c, void *a, long n, vlong offset)
{
	ulong port;
	char buf[256];

	switch(TYPE(c->qid)){
	case Qpnpctl:
		if(n >= sizeof(buf))
			n = sizeof(buf)-1;
		strncpy(buf, a, n);
		buf[n] = 0;
		if(strncmp(buf, "rddata ", 7) == 0) {
			port = strtoul(buf+7, 0, 0);
			if(port < 0x203 || port > 0x3ff)
				error("bad value for rddata port");
			if(!pnpscan(port))
				error("no cards found");
		}
		else if(strncmp(buf, "debug ", 6) == 0)
			pnp.debug = strtoul(buf+6, 0, 0);
		else
			error(Ebadctl);
		break;
	default:
USED(offset);
		error(Egreg);
	}
	return n;
}

Dev pnpdevtab = {
	'$',
	"pnp",

	pnpreset,
	devinit,
	pnpattach,
	pnpwalk,
	pnpstat,
	pnpopen,
	devcreate,
	pnpclose,
	pnpread,
	devbread,
	pnpwrite,
	devbwrite,
	devremove,
	devwstat,
};
