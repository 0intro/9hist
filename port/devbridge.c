#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/netif.h"
#include "../port/error.h"

typedef struct Bridge 	Bridge;
typedef struct Port 	Port;
typedef struct Centry	Centry;

enum
{
	Qtopdir=	1,		/* top level directory */

	Qbridgedir,			/* bridge* directory */
	Qbctl,
	Qstats,
	Qcache,
	Qlog,

	Qportdir,			/* directory for a protocol */
	Qpctl,
	Qlocal,
	Qstatus,

	MaxQ,

	Maxbridge=	4,
	Maxport=	16,		// power of 2
	CacheHash=	257,		// prime
	CacheLook=	5,		// how many cache entries to examine
	CacheSize=	(CacheHash+CacheLook-1),
	CacheTimeout=	5*60,		// timeout for cache entry in seconds

	Addrlen=	16,		// must be long enough of IP addr and ether addr
};

Dirtab bridgedirtab[]={
	"ctl",		{Qbctl},	0,	0666,
	"stats",	{Qstats},	0,	0444,
	"cache",	{Qcache},	0,	0444,
	"log",		{Qlog},		0,	0666,
};

Dirtab portdirtab[]={
	"ctl",		{Qpctl},	0,	0666,
	"local",	{Qlocal},	0,	0444,
	"status",	{Qstatus},	0,	0444,
};

enum {
	Logcache=	(1<<0),
	Logmcast=	(1<<1),
};

// types of interfaces
enum
{
	Tether,
	Ttunnel,
};

static Logflag logflags[] =
{
	{ "cache",	Logcache, },
	{ "multicast",	Logmcast, },
	{ nil,		0, },
};

static Dirtab	*dirtab[MaxQ];

#define TYPE(x) 	((x).path & 0xff)
#define PORT(x) 	(((x).path >> 8)&(Maxport-1))
#define QID(x, y) 	(((x)<<8) | (y))

struct Centry
{
	uchar	d[Eaddrlen];
	int	port;
	long	expire;		// entry expires this number of seconds after bootime
	long	src;
	long	dst;
};

struct Bridge
{
	QLock;
	int	nport;
	Port	*port[Maxport];
	Centry	cache[CacheSize];
	int	hit;
	int	miss;

	Log;
};

struct Port
{
	int	id;
	Bridge	*bridge;
	int	ref;
	int	closed;

	Chan	*data[2];	// channel to data

	int	mcast;		// send multi cast packets
	Proc	*readp;		// read proc
	
	// the following uniquely identifies the port
	int	type;
	uchar	addr[Addrlen];
	
	// owner hash - avoids bind/unbind races
	ulong	ownhash;

	// various stats
	int	in;		// number of packets read
	int	inmulti;	// multicast or broadcast
	int	inunknown;	// unknown address
	int	out;		// number of packets read
	int	outmulti;	// multicast or broadcast
	int	outunknown;	// unknown address
	int	nentry;		// number of cache entries for this port
};

static Bridge bridgetab[Maxbridge];

static int m2p[] = {
	[OREAD]		4,
	[OWRITE]	2,
	[ORDWR]		6
};

static int	bridgegen(Chan *c, Dirtab*, int, int s, Dir *dp);
static void	portbind(Bridge *b, int argc, char *argv[]);
static void	portunbind(Bridge *b, int argc, char *argv[]);
static void	etherread(void *a);
static char	*cachedump(Bridge *b);
static void	portfree(Port *port);
static void	cacheflushport(Bridge *b, int port);

extern ulong	parseip(uchar*, char*);

static void
bridgeinit(void)
{
	int i;
	Dirtab *dt;
	// setup dirtab with non directory entries
	for(i=0; i<nelem(bridgedirtab); i++) {
		dt = bridgedirtab + i;
		dirtab[TYPE(dt->qid)] = dt;
	}
	for(i=0; i<nelem(portdirtab); i++) {
		dt = portdirtab + i;
		dirtab[TYPE(dt->qid)] = dt;
	}
}

static Chan*
bridgeattach(char* spec)
{
	Chan *c;
	int dev;

	dev = atoi(spec);
	if(dev<0 || dev >= Maxbridge)
		error("bad specification");

	c = devattach('B', spec);
	c->qid = (Qid){QID(0, Qtopdir)|CHDIR, 0};
	c->dev = dev;

	return c;
}

static int
bridgewalk(Chan *c, char *name)
{
	Path *op;

	if(strcmp(name, "..") == 0){
		switch(TYPE(c->qid)){
		case Qtopdir:
		case Qbridgedir:
			c->qid = (Qid){CHDIR|Qtopdir, 0};
			break;
		case Qportdir:
			c->qid = (Qid){CHDIR|Qbridgedir, 0};
			break;
		default:
			panic("bridgewalk %lux", c->qid.path);
		}
		op = c->path;
		c->path = ptenter(&syspt, op, name);
		decref(op);
		return 1;
	}

	return devwalk(c, name, 0, 0, bridgegen);
}

static void
bridgestat(Chan* c, char* db)
{
	devstat(c, db, nil, 0, bridgegen);
}

static Chan*
bridgeopen(Chan* c, int omode)
{
	int perm;
	Bridge *b;

	omode &= 3;
	perm = m2p[omode];
	USED(perm);

	b = bridgetab + c->dev;
	USED(b);

	switch(TYPE(c->qid)) {
	default:
		break;
	case Qtopdir:
	case Qbridgedir:
	case Qportdir:
	case Qstatus:
	case Qlocal:
	case Qstats:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qlog:
		logopen(b);
		break;
	case Qcache:
		if(omode != OREAD)
			error(Eperm);
		c->aux = cachedump(b);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
bridgeclose(Chan* c)
{
	Bridge *b  = bridgetab + c->dev;

	switch(TYPE(c->qid)) {
	case Qcache:
		if(c->flag & COPEN)
			free(c->aux);
		break;
	case Qlog:
		if(c->flag & COPEN)
			logclose(b);
		break;
	}
}

static long
bridgeread(Chan *c, void *a, long n, vlong off)
{
	char buf[256];
	Bridge *b = bridgetab + c->dev;
	Port *port;
	int i;

	USED(off);
	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qtopdir:
	case Qbridgedir:
	case Qportdir:
		return devdirread(c, a, n, 0, 0, bridgegen);
	case Qlog:
		return logread(b, a, off, n);
	case Qstatus:
		qlock(b);
		port = b->port[PORT(c->qid)];
		if(port == 0)
			strcpy(buf, "unbound\n");
		else {
			i = 0;
			switch(port->type) {
			default: panic("bridgeread: unknown port type: %d", port->type);
			case Tether:
				i += snprint(buf+i, sizeof(buf)-i, "ether %E: ", port->addr);
				break;
			case Ttunnel:
				i += snprint(buf+i, sizeof(buf)-i, "tunnel %I: ", port->addr);
				break;
			}
			i += snprint(buf+i, sizeof(buf)-i, "in=%d/%d/%d out=%d/%d/%d\n",
				port->in, port->inmulti, port->inunknown,
				port->out, port->outmulti, port->outunknown);
			USED(i);
		}
		n = readstr(off, a, n, buf);
		qunlock(b);
		return n;
	case Qcache:
		n = readstr(off, a, n, c->aux);
		return n;
	}
}

static long
bridgewrite(Chan *c, void *a, long n, vlong off)
{
	Bridge *b = bridgetab + c->dev;
	Cmdbuf *cb;
	char *arg0;
	char *p;
	
	USED(off);

	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qbctl:
		cb = parsecmd(a, n);
		qlock(b);
		if(waserror()) {
			qunlock(b);
			free(cb);
			nexterror();
		}
		if(cb->nf == 0)
			error("short write");
		arg0 = cb->f[0];
		if(strcmp(arg0, "bind") == 0)
			portbind(b, cb->nf-1, cb->f+1);
		else if(strcmp(arg0, "unbind") == 0)
			portunbind(b, cb->nf-1, cb->f+1);
		else if(strcmp(arg0, "cacheflush") == 0) {
			log(b, Logcache, "cache flush\n");
			memset(b->cache, 0, CacheSize*sizeof(Centry));
		} else
			error("unknown control request");
		poperror();
		qunlock(b);
		free(cb);
		return n;
	case Qlog:
		cb = parsecmd(a, n);
		p = logctl(b, cb->nf, cb->f, logflags);
		free(cb);
		if(p != nil)
			error(p);
		return n;
	}
}

static int
bridgegen(Chan *c, Dirtab*, int, int s, Dir *dp)
{
	Bridge *b = bridgetab + c->dev;
	int type = TYPE(c->qid);
	char buf[32];
	Dirtab *dt;
	Qid qid;

	switch(type) {
	default:
		// non directory entries end up here
		if(c->qid.path & CHDIR)
			panic("bridgegen: unexpected directory");	
		if(s != 0)
			return -1;
		dt = dirtab[TYPE(c->qid)];
		if(dt == nil)
			panic("bridgegen: unknown type: %d", TYPE(c->qid));
		devdir(c, c->qid, dt->name, dt->length, eve, dt->perm, dp);
		return 1;
	case Qtopdir:
		if(s != 0)
			return -1;
		sprint(buf, "bridge%ld", c->dev);
		devdir(c, (Qid){QID(0,Qbridgedir)|CHDIR,0}, buf, 0, eve, 0555, dp);
		return 1;
	case Qbridgedir:
		if(s<nelem(bridgedirtab)) {
			dt = bridgedirtab+s;
			devdir(c, dt->qid, dt->name, dt->length, eve, dt->perm, dp);
			return 1;
		}
		s -= nelem(bridgedirtab);
		if(s >= b->nport)
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

// also in netif.c
static int
parseaddr(uchar *to, char *from, int alen)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for(i = 0; i < alen; i++){
		if(*p == 0)
			return -1;
		nip[0] = *p++;
		if(*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
		if(*p == ':')
			p++;
	}
	return 0;
}

// assumes b is locked
static void
portbind(Bridge *b, int argc, char *argv[])
{
	Port *port;
	char path[8*NAMELEN];
	char buf[100];
	char *dev, *dev2=nil, *p;
	Chan *ctl;
	int type=0, i, n;
	char *usage = "usage: bind ether|tunnel addr ownhash dev [dev2]";
	uchar addr[Addrlen];
	ulong ownhash;

	memset(addr, 0, Addrlen);
	if(argc < 4)
		error(usage);
	if(strcmp(argv[0], "ether") == 0) {
		if(argc != 4)
			error(usage);
		type = Tether;
		parseaddr(addr, argv[1], Eaddrlen);
	} else if(strcmp(argv[0], "tunnel") == 0) {
		if(argc != 5)
			error(usage);
		type = Ttunnel;
		parseip(addr, argv[1]);
		dev2 = argv[4];
	} else
		error(usage);
	ownhash = atoi(argv[2]);
	dev = argv[3];
	for(i=0; i<b->nport; i++) {
		port = b->port[i];
		if(port != nil)
		if(port->type == type)
		if(memcmp(port->addr, addr, Addrlen) == 0)
			error("port in use");
	}
	for(i=0; i<Maxport; i++)
		if(b->port[i] == nil)
			break;
	if(i == Maxport)
		error("no more ports");
	port = smalloc(sizeof(Port));
	port->ref = 1;
	port->id = i;
	port->ownhash = ownhash;

	if(waserror()) {
		portfree(port);
		nexterror();
	}

	port->type = type;
	memmove(port->addr, addr, Addrlen);
	switch(port->type) {
	default: panic("portbind: unknown port type: %d", type);
	case Tether:
		snprint(path, sizeof(path), "%s/clone", dev);
		ctl = namec(path, Aopen, ORDWR, 0);
		if(waserror()) {
			cclose(ctl);
			nexterror();
		}
		// check addr?

		// get directory name
		n = devtab[ctl->type]->read(ctl, buf, sizeof(buf), 0);
		buf[n] = 0;
		for(p = buf; *p == ' '; p++)
			;
		snprint(path, sizeof(path), "%s/%lud/data", dev, strtoul(p, 0, 0));

		// setup connection to be promiscuous
		snprint(buf, sizeof(buf), "connect -1");
		devtab[ctl->type]->write(ctl, buf, strlen(buf), 0);
		snprint(buf, sizeof(buf), "promiscuous");
		devtab[ctl->type]->write(ctl, buf, strlen(buf), 0);

		// open data port
		port->data[0] = namec(path, Aopen, ORDWR, 0);
		// dup it
		incref(port->data[0]);
		port->data[1] = port->data[0];

		poperror();
		cclose(ctl);		

		break;
	case Ttunnel:
		port->data[0] = namec(dev, Aopen, OREAD, 0);
		port->data[1] = namec(dev2, Aopen, OWRITE, 0);
		break;
	}

	poperror();

	// commited to binding port
	b->port[port->id] = port;
	port->bridge = b;
	if(b->nport <= port->id)
		b->nport = port->id+1;

	// assumes kproc always succeeds
	port->ref++;
	kproc("etherread", etherread, port);	// poperror must be next
}

// assumes b is locked
static void
portunbind(Bridge *b, int argc, char *argv[])
{
	Port *port=nil;
	int type=0, i;
	char *usage = "usage: unbind ether|tunnel addr [ownhash]";
	uchar addr[Addrlen];
	ulong ownhash;

	memset(addr, 0, Addrlen);
	if(argc < 2 || argc > 3)
		error(usage);
	if(strcmp(argv[0], "ether") == 0) {
		type = Tether;
		parseaddr(addr, argv[1], Eaddrlen);
	} else if(strcmp(argv[0], "tunnel") == 0) {
		type = Ttunnel;
		parseip(addr, argv[1]);
	} else
		error(usage);
	if(argc == 3)
		ownhash = atoi(argv[2]);
	else
		ownhash = 0;
	for(i=0; i<b->nport; i++) {
		port = b->port[i];
		if(port != nil)
		if(port->type == type)
		if(memcmp(port->addr, addr, Addrlen) == 0)
			break;
	}
	if(i == b->nport)
		error("port not found");
	if(ownhash != 0 && port->ownhash != 0 && ownhash != port->ownhash)
		error("bad owner hash");

	port->closed = 1;
	b->port[i] = nil;	// port is now unbound
	cacheflushport(b, i);

	// try and stop reader
	if(port->readp)
		postnote(port->readp, 1, "unbind", 0);
	portfree(port);
}

// assumes b is locked
static Centry *
cachelookup(Bridge *b, uchar d[Eaddrlen])
{
	int i;
	uint h;
	Centry *p;
	long sec;

	// dont cache multicast or broadcast
	if(d[0] & 1)
		return 0;

	h = 0;
	for(i=0; i<Eaddrlen; i++) {
		h *= 7;
		h += d[i];
	}
	h %= CacheHash;
	p = b->cache + h;
	sec = TK2SEC(m->ticks);
	for(i=0; i<CacheLook; i++,p++) {
		if(memcmp(d, p->d, Eaddrlen) == 0) {
			p->dst++;
			if(sec >= p->expire) {
				log(b, Logcache, "expired cache entry: %E %d\n",
					d, p->port);
				return nil;
			}
			p->expire = sec + CacheTimeout;
			return p;
		}
	}
	log(b, Logcache, "cache miss: %E\n", d);
	return nil;
}

// assumes b is locked
static void
cacheupdate(Bridge *b, uchar d[Eaddrlen], int port)
{
	int i;
	uint h;
	Centry *p, *pp;
	long sec;

	// dont cache multicast or broadcast
	if(d[0] & 1) {
		log(b, Logcache, "bad source address: %E\n", d);
		return;
	}
	
	h = 0;
	for(i=0; i<Eaddrlen; i++) {
		h *= 7;
		h += d[i];
	}
	h %= CacheHash;
	p = b->cache + h;
	pp = p;
	sec = p->expire;

	// look for oldest entry
	for(i=0; i<CacheLook; i++,p++) {
		if(memcmp(p->d, d, Eaddrlen) == 0) {
			p->expire = TK2SEC(m->ticks) + CacheTimeout;
			if(p->port != port) {
				log(b, Logcache, "NIC changed port %d->%d: %E\n",
					p->port, port, d);
				p->port = port;
			}
			p->src++;
			return;
		}
		if(p->expire < sec) {
			sec = p->expire;
			pp = p;
		}
	}
	if(pp->expire != 0)
		log(b, Logcache, "bumping from cache: %E %d\n", pp->d, pp->port);
	pp->expire = TK2SEC(m->ticks) + CacheTimeout;
	memmove(pp->d, d, Eaddrlen);
	pp->port = port;
	pp->src = 1;
	pp->dst = 0;
	log(b, Logcache, "adding to cache: %E %d\n", pp->d, pp->port);
}

// assumes b is locked
static void
cacheflushport(Bridge *b, int port)
{
	Centry *ce;
	int i;

	ce = b->cache;
	for(i=0; i<CacheSize; i++,ce++) {
		if(ce->port != port)
			continue;
		memset(ce, 0, sizeof(Centry));
	}
}

static char *
cachedump(Bridge *b)
{
	int i, n;
	long sec, off;
	char *buf, *p, *ep;
	Centry *ce;
	char c;

	qlock(b);
	if(waserror()) {
		qunlock(b);
		nexterror();
	}
	sec = TK2SEC(m->ticks);
	n = 0;
	for(i=0; i<CacheSize; i++)
		if(b->cache[i].expire != 0)
			n++;
	
	n *= 51;	// change if print format is changed
	n += 10;	// some slop at the end
	buf = malloc(n);
	p = buf;
	ep = buf + n;
	ce = b->cache;
	off = seconds() - sec;
	for(i=0; i<CacheSize; i++,ce++) {
		if(ce->expire == 0)
			continue;	
		c = (sec < ce->expire)?'v':'e';
		p += snprint(p, ep-p, "%E %2d %10ld %10ld %10ld %c\n", ce->d,
			ce->port, ce->src, ce->dst, ce->expire+off, c);
	}
	*p = 0;
	poperror();
	qunlock(b);

	return buf;
}



// assumes b is locked
static void
ethermultiwrite(Bridge *b, Block *bp, Port *port)
{
	Chan *c;
	Block *bp2;
	Etherpkt *ep;
	int i, mcast, bcast;
	static uchar bcastaddr[Eaddrlen] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if(waserror()) {
		if(bp)
			freeb(bp);
		nexterror();
	}
	
	ep = (Etherpkt*)bp->rp;
	mcast = ep->d[0] & 1;
	if(mcast)
		bcast = memcmp(ep->d, bcastaddr, Eaddrlen) == 0;
	else
		bcast = 0;

	c = nil;
	for(i=0; i<b->nport; i++) {
		if(i == port->id || b->port[i] == nil)
			continue;
		if(mcast && !bcast && !b->port[i]->mcast)
			continue;
		b->port[i]->out++;
		if(mcast)
			b->port[i]->outmulti++;
		else
			b->port[i]->outunknown++;

		// delay one so that the last write does not copy
		if(c != nil) {
			// can not use write since it changes scr addr
			bp2 = copyblock(bp, blocklen(bp));
			devtab[c->type]->bwrite(c, bp2, 0);
		}
		c = b->port[i]->data[1];
	}

	// last write free block
	if(c) {
		bp2 = bp; bp = nil; USED(bp);
		devtab[c->type]->bwrite(c, bp2, 0);
	} else
		freeb(bp);

	poperror();
}

/*
 *  process to read from the ethernet
 */
static void
etherread(void *a)
{
	Port *port = a, *oport;
	Bridge *b = port->bridge;
	Block *bp, *bp2;
	Etherpkt *ep;
	Centry *ce;

	
	qlock(b);
	port->readp = up;	/* hide identity under a rock for unbind */

	while(!port->closed){
		// release lock to read - error means it is time to quit
		qunlock(b);
		if(waserror()) {
			qlock(b);
			break;
		}
//print("devbridge: etherread: reading\n");
		bp = devtab[port->data[0]->type]->bread(port->data[0], ETHERMAXTU, 0);
//print("devbridge: etherread: blocklen = %d\n", blocklen(bp));
		poperror();
		qlock(b);

		if(port->closed)
			break;
		if(waserror()) {
			if(bp)
				freeb(bp);
print("devbridge: etherread: %r\n");
			continue;
		}
		if(blocklen(bp) < ETHERMINTU)
			error("short packet");
		port->in++;
		ep = (Etherpkt*)bp->rp;
		cacheupdate(b, ep->s, port->id);

		if(ep->d[0] & 1) {
			log(b, Logmcast, "mulitcast: port=%d src=%E dst=%E type=%#.4ux\n",
				port->id, ep->s, ep->d, (ep->type[0]<<8)|ep->type[1] );
			port->inmulti++;
			bp2 = bp; bp = nil;
			ethermultiwrite(b, bp2, port);
		} else {
			ce = cachelookup(b, ep->d);
			if(ce == nil) {
				b->miss++;
				port->inunknown++;
				bp2 = bp; bp = nil;
				ethermultiwrite(b, bp2, port);
			} else if (ce->port != port->id) {
				b->hit++;
				bp2 = bp; bp = nil;
				oport = b->port[ce->port];
				oport->out++;
				devtab[oport->data[1]->type]->bwrite(oport->data[1], bp2, 0);
			}
		}

		poperror();
		if(bp)
			freeb(bp);
	}
print("etherread: trying to exit\n");
	port->readp = nil;
	portfree(port);
	qunlock(b);
	pexit("hangup", 1);
}

// hold b lock
static void
portfree(Port *port)
{
	port->ref--;
	if(port->ref < 0)
		panic("portfree: bad ref");
	if(port->ref > 0)
		return;

	if(port->data[0])
		cclose(port->data[0]);
	if(port->data[1])
		cclose(port->data[1]);
	memset(port, 0, sizeof(Port));
	free(port);
}

Dev bridgedevtab = {
	'B',
	"bridge",

	devreset,
	bridgeinit,
	bridgeattach,
	devclone,
	bridgewalk,
	bridgestat,
	bridgeopen,
	devcreate,
	bridgeclose,
	bridgeread,
	devbread,
	bridgewrite,
	devbwrite,
	devremove,
	devwstat,
};