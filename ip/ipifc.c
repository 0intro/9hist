#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

#define DPRINT if(0)print

enum {
	Maxmedia	= 16,
	Nself		= Maxmedia*5,
	NHASH		= (1<<6),
	NCACHE		= 256,
	QMAX		= 64*1024-1,
};

	Proto	ipifc;
extern	Fs	fs;

Medium *media[] =
{
	&ethermedium,
	&pktmedium,
	&nullmedium,
	0
};

/*
 *  cache of local addresses (addresses we answer to)
 */
typedef struct Ipself Ipself;
struct Ipself
{
	uchar	a[IPaddrlen];
	Ipself	*hnext;		/* next address in the hash table */
	Iplink	*link;		/* binding twixt Ipself and Ipifc */
	ulong	expire;
	uchar	type;		/* type of address */
	int	ref;
	Ipself	*next;		/* free list */
};

typedef struct Ipselftab Ipselftab;
struct Ipselftab
{
	QLock;
	int	inited;
	int	acceptall;	/* true if an interface has the null address */
	Ipself	*hash[NHASH];	/* hash chains */
};
Ipselftab	selftab;

/*
 *  Multicast addresses are chained onto a Chan so that
 *  we can remove them when the Chan is closed.
 */
typedef struct Ipmcast Ipmcast;
struct Ipmcast
{
	Ipmcast	*next;
	uchar	ma[IPaddrlen];	/* multicast address */
	uchar	ia[IPaddrlen];	/* interface address */
};

/* quick hash for ip addresses */
#define hashipa(a) ( ( ((a)[IPaddrlen-2]<<8) | (a)[IPaddrlen-1] )%NHASH )

static char tifc[] = "ifc ";

static void	addselfcache(Ipifc *ifc, Iplifc *lifc, uchar *a, int type);
static void	remselfcache(Ipifc *ifc, Iplifc *lifc, uchar *a);
static char*	ipifcjoinmulti(Ipifc *ifc, char **argv, int argc);
static char*	ipifcleavemulti(Ipifc *ifc, char **argv, int argc);

/*
 *  find the medium with this name
 */
Medium*
ipfindmedium(char *name)
{
	Medium **mp;

	for(mp = media; *mp != nil; mp++)
		if(strcmp((*mp)->name, name) == 0)
			break;
	return *mp;
}

/*
 *  attach a device (or pkt driver) to the interface.
 *  called with c->car locked
 */
static char*
ipifcbind(Conv *c, char **argv, int argc)
{
	Ipifc *ifc;
	Medium *m;

	if(argc < 2)
		return Ebadarg;

	ifc = (Ipifc*)c->ptcl;

	/* bind the device to the interface */
	m = ipfindmedium(argv[1]);
	if(m == nil)
		return "unknown interface type";

	wlock(ifc);
	if(ifc->m != nil){
		wunlock(ifc);
		return "interface already bound";	
	}
	if(waserror()){
		wunlock(ifc);
		nexterror();
	}

	(*m->bind)(ifc, argc, argv);
	if(argc > 2)
		strncpy(ifc->dev, argv[2], sizeof(ifc->dev));
	else
		sprint(ifc->dev, "%s%d", m->name, c->x);
	ifc->dev[sizeof(ifc->dev)-1] = 0;
	ifc->m = m;
	ifc->minmtu = ifc->m->minmtu;
	ifc->maxmtu = ifc->m->maxmtu;
	ifc->ifcid++;

	wunlock(ifc);
	poperror();

	return nil;
}

/*
 *  detach a device from an interface, close the interface
 */
static char*
ipifcunbind(Ipifc *ifc)
{
	char *av[4];
	char ip[32];
	char mask[32];

	if(waserror()){
		wunlock(ifc);
		nexterror();
	}
	wlock(ifc);

	/* dissociate routes */
	ifc->ifcid++;

	/* disassociate device */
	(*ifc->m->unbind)(ifc);
	memset(ifc->dev, 0, sizeof(ifc->dev));
	ifc->arg = nil;

	/* hangup queues to stop queuing of packets */
	qhangup(ifc->conv->rq, "unbind");
	qhangup(ifc->conv->wq, "unbind");

	/* disassociate logical interfaces */
	av[0] = "remove";
	av[1] = ip;
	av[2] = mask;
	av[3] = 0;
	while(ifc->lifc){
		sprint(ip, "%I", ifc->lifc->local);
		sprint(mask, "%M", ifc->lifc->mask);
		ipifcrem(ifc, av, 3, 0);
	}

	ifc->m = nil;
	wunlock(ifc);
	poperror();
	return nil;
}

static int
ipifcstate(Conv *c, char *state, int n)
{
	Ipifc *ifc;
	Iplifc *lifc;
	int m;

	ifc = (Ipifc*)c->ptcl;

	m = snprint(state, n, "%-12.12s %-5d", ifc->dev, ifc->maxmtu);

	rlock(ifc);
	for(lifc = ifc->lifc; lifc; lifc = lifc->next)
		m += snprint(state+m, n - m,
			" %-20.20I %-20.20M %-20.20I %-7d %-7d %-7d %-7d",
				lifc->local, lifc->mask, lifc->remote,
				ifc->in, ifc->out, ifc->inerr, ifc->outerr);
	m += snprint(state+m, n - m, "\n");
	runlock(ifc);
	return m;
}

static int
ipifclocal(Conv *c, char *state, int n)
{
	Ipifc *ifc;
	Iplifc *lifc;
	Iplink *link;
	int m;

	ifc = (Ipifc*)c->ptcl;

	m = 0;

	rlock(ifc);
	for(lifc = ifc->lifc; lifc; lifc = lifc->next){
		m += snprint(state+m, n - m, "%-20.20I ->", lifc->local);
		for(link = lifc->link; link; link = link->lifclink)
			m += snprint(state+m, n - m, " %-20.20I", link->self->a);
		m += snprint(state+m, n - m, "\n");
	}
	runlock(ifc);
	return m;
}

static int
ipifcinuse(Conv *c)
{
	Ipifc *ifc;

	ifc = (Ipifc*)c->ptcl;
	return ifc->m != nil;
}

/*
 *  called when a process writes to an interface's 'data'
 */
static void
ipifckick(Conv *c, int)
{
	Block *bp;
	Ipifc *ifc;

	bp = qget(c->wq);
	if(bp == nil)
		return;

	ifc = (Ipifc*)c->ptcl;
	if(ifc->m == nil || ifc->m->pktin == nil)
		freeb(bp);
	else
		(*ifc->m->pktin)(ifc, bp);
}

/*
 *  we'll have to have a kick routine at
 *  some point to deal with these
 */
static void
ipifccreate(Conv *c)
{
	Ipifc *ifc;

	c->rq = qopen(QMAX, 0, 0, 0);
	c->wq = qopen(QMAX, 0, 0, 0);
	ifc = (Ipifc*)c->ptcl;
	ifc->conv = c;
	ifc->unbinding = 0;
	ifc->m = nil;
}

/* 
 *  called after last close of ipifc data or ctl
 *  called with c locked, we must unlock
 */
static void
ipifcclose(Conv *c)
{
	Ipifc *ifc;
	Medium *m;

	ifc = (Ipifc*)c->ptcl;
	m = ifc->m;
	if(m != nil && m->unbindonclose)
		ipifcunbind(ifc);
	unlock(c);
}

/*
 *  add an address to an interface.
 */
char*
ipifcadd(Ipifc *ifc, char **argv, int argc)
{
	uchar ip[IPaddrlen], mask[IPaddrlen], rem[IPaddrlen];
	uchar bcast[IPaddrlen], net[IPaddrlen];
	Iplifc *lifc, **l;
	int i, type, mtu;

	memset(ip, 0, IPaddrlen);
	memset(mask, 0, IPaddrlen);
	memset(rem, 0, IPaddrlen);
	switch(argc){
	case 5:
		mtu = strtoul(argv[4], 0, 0);
		if(mtu >= ifc->m->minmtu && mtu <= ifc->m->maxmtu)
			ifc->maxmtu = mtu;
		/* fall through */
	case 4:
		parseip(ip, argv[1]);
		parseipmask(mask, argv[2]);
		parseip(rem, argv[3]);
		maskip(rem, mask, net);
		break;
	case 3:
		parseip(ip, argv[1]);
		parseipmask(mask, argv[2]);
		maskip(ip, mask, rem);
		maskip(rem, mask, net);
		break;
	case 2:
		parseip(ip, argv[1]);
		memmove(mask, defmask(ip), IPaddrlen);
		maskip(ip, mask, rem);
		maskip(rem, mask, net);
		break;
	default:
		return Ebadarg;
		break;
	}

	if(waserror()){
		wunlock(ifc);
		panic("ipifcadd");
	}
	wlock(ifc);

	/* ignore if this is already a local address for this ifc */
	for(lifc = ifc->lifc; lifc; lifc = lifc->next)
		if(ipcmp(lifc->local, ip) == 0)
			goto out;

	/* add the address to the list of logical ifc's for this ifc */
	lifc = smalloc(sizeof(Iplifc));
	ipmove(lifc->local, ip);
	ipmove(lifc->mask, mask);
	ipmove(lifc->remote, rem);
	ipmove(lifc->net, net);
	lifc->next = nil;
	for(l = &ifc->lifc; *l; l = &(*l)->next)
		;
	*l = lifc;

	/* add a route for the local network */
	type = Rifc;
	if(ipcmp(mask, IPallbits) == 0)
		type |= Rptpt;
	if(isv4(ip))
		v4addroute(tifc, rem+IPv4off, mask+IPv4off, ip+IPv4off, type);
	else
		v6addroute(tifc, ip, mask, rem, type);

	addselfcache(ifc, lifc, ip, Runi);

	/* add subnet directed broadcast addresses to the self cache */
	for(i = 0; i < IPaddrlen; i++)
		bcast[i] = (ip[i] & mask[i]) | ~mask[i];
	addselfcache(ifc, lifc, bcast, Rbcast);

	/* add network directed broadcast addresses to the self cache */
	memmove(mask, defmask(ip), IPaddrlen);
	for(i = 0; i < IPaddrlen; i++)
		bcast[i] = (ip[i] & mask[i]) | ~mask[i];
	addselfcache(ifc, lifc, bcast, Rbcast);

	addselfcache(ifc, lifc, IPv4bcast, Rbcast);

out:
	wunlock(ifc);
	poperror();
	return nil;
}

/*
 *  remove an address from an interface.
 *  called with c->car locked
 */
char*
ipifcrem(Ipifc *ifc, char **argv, int argc, int dolock)
{
	uchar ip[IPaddrlen];
	uchar mask[IPaddrlen];
	Iplifc *lifc, **l;

	if(argc < 3)
		return Ebadarg;

	parseip(ip, argv[1]);
	parseipmask(mask, argv[2]);

	if(dolock){
		if(waserror()){
			wunlock(ifc);
			nexterror();
		}
		wlock(ifc);
	}

	/* find address on this interface and remove from chain */
	lifc = nil;
	for(l = &ifc->lifc; *l; l = &(*l)->next)
		if(memcmp(ip, (*l)->local, IPaddrlen) == 0)
		if(memcmp(mask, (*l)->mask, IPaddrlen) == 0){
			lifc = *l;
			*l = lifc->next;
			break;
		}

	if(lifc == nil)
		return "address not on this interface";

	/* disassociate any addresses */
	while(lifc->link)
		remselfcache(ifc, lifc, lifc->link->self->a);

	/* remove the route for this logical interface */
	if(isv4(ip))
		v4delroute(lifc->remote+IPv4off, lifc->mask+IPv4off);
	else
		v6delroute(lifc->remote, lifc->mask);

	free(lifc);

out:
	if(dolock){
		wunlock(ifc);
		poperror();
	}
	return nil;
}

/*
 * distrbute routes to active interfaces like the
 * TRIP linecards
 */
void
ipifcaddroute(int vers, uchar *addr, uchar *mask, uchar *gate, int type)
{
	Medium *m;
	Conv **cp;
	Ipifc *ifc;

	for(cp = ipifc.conv; cp < &ipifc.conv[ipifc.nc]; cp++){
		if(*cp != nil) {
			ifc = (Ipifc*)(*cp)->ptcl;
			m = ifc->m;
			if(m == nil)
				continue;
			if(m->addroute != nil)
				m->addroute(ifc, vers, addr, mask, gate, type);
		}
	}
}

void
ipifcremroute(int vers, uchar *addr, uchar *mask)
{
	Medium *m;
	Conv **cp;
	Ipifc *ifc;

	for(cp = ipifc.conv; cp < &ipifc.conv[ipifc.nc]; cp++){
		if(*cp != nil) {
			ifc = (Ipifc*)(*cp)->ptcl;
			m = ifc->m;
			if(m == nil)
				continue;
			if(m->remroute != nil)
				m->remroute(ifc, vers, addr, mask);
		}
	}
}

/*
 *  associate an address with the interface.  This wipes out any previous
 *  addresses.  This is a macro that means, remove all the old interfaces
 *  and add a new one.
 */
static char*
ipifcconnect(Conv* c, char **argv, int argc)
{
	char *err;
	Ipifc *ifc;
	char *av[4];
	char ip[80], mask[80];

	ifc = (Ipifc*)c->ptcl;

	if(ifc->m == nil)
		 return "ipifc not yet bound to device";

	av[0] = "remove";
	av[1] = ip;
	av[2] = mask;
	av[3] = 0;
	if(waserror()){
		wunlock(ifc);
		nexterror();
	}
	wlock(ifc);
	while(ifc->lifc){
		sprint(ip, "%I", ifc->lifc->local);
		sprint(mask, "%I", ifc->lifc->mask);
		ipifcrem(ifc, av, 3, 0);
	}
	wunlock(ifc);
	poperror();

	err = ipifcadd(ifc, argv, argc);
	if(err)
		return err;

	Fsconnected(&fs, c, nil);

	return nil;
}

/*
 *  non-standard control messages.
 *  called with c->car locked.
 */
static char*
ipifcctl(Conv* c, char**argv, int argc)
{
	Ipifc *ifc;

	ifc = (Ipifc*)c->ptcl;
	if(strcmp(argv[0], "add") == 0)
		return ipifcadd(ifc, argv, argc);
	else if(strcmp(argv[0], "remove") == 0)
		return ipifcrem(ifc, argv, argc, 1);
	else if(strcmp(argv[0], "unbind") == 0)
		return ipifcunbind(ifc);
	else if(strcmp(argv[0], "joinmulti") == 0)
		return ipifcjoinmulti(ifc, argv, argc);
	else if(strcmp(argv[0], "leavemulti") == 0)
		return ipifcleavemulti(ifc, argv, argc);
	return "unsupported ctl";
}

void
ipifcinit(Fs *fs)
{
	ipifc.name = "ipifc";
	ipifc.kick = ipifckick;
	ipifc.connect = ipifcconnect;
	ipifc.announce = nil;
	ipifc.bind = ipifcbind;
	ipifc.state = ipifcstate;
	ipifc.create = ipifccreate;
	ipifc.close = ipifcclose;
	ipifc.rcv = nil;
	ipifc.ctl = ipifcctl;
	ipifc.advise = nil;
	ipifc.stats = ipstats;
	ipifc.inuse = ipifcinuse;
	ipifc.local = ipifclocal;
	ipifc.ipproto = -1;
	ipifc.nc = Maxmedia;
	ipifc.ptclsize = sizeof(Ipifc);

	Fsproto(fs, &ipifc);
}

/*
 *  add to self routing cache
 *	called with c->car locked
 */
static void
addselfcache(Ipifc *ifc, Iplifc *lifc, uchar *a, int type)
{
	Ipself *p;
	Iplink *lp;
	int h;

	qlock(&selftab);

	/* see if the address already exists */
	h = hashipa(a);
	for(p = selftab.hash[h]; p; p = p->next)
		if(memcmp(a, p->a, IPaddrlen) == 0)
			break;

	/* allocate a local address and add to hash chain */
	if(p == nil){
		p = smalloc(sizeof(*p));
		ipmove(p->a, a);
		p->type = type;
		p->next = selftab.hash[h];
		selftab.hash[h] = p;

		/* if the null address, accept all packets */
		if(ipcmp(a, v4prefix) == 0 || ipcmp(a, IPnoaddr) == 0)
			selftab.acceptall = 1;
	}

	/* look for a link for this lifc */
	for(lp = p->link; lp; lp = lp->selflink)
		if(lp->lifc == lifc)
			break;

	/* allocate a lifc-to-local link and link to both */
	if(lp == nil){
		lp = smalloc(sizeof(*lp));
		lp->ref = 1;
		lp->lifc = lifc;
		lp->self = p;
		lp->selflink = p->link;
		p->link = lp;
		lp->lifclink = lifc->link;
		lifc->link = lp;

		/* add to routing table */
		if(isv4(a))
			v4addroute(tifc, a+IPv4off, IPallbits+IPv4off, a+IPv4off, type);
		else
			v6addroute(tifc, a, IPallbits, a, type);

		if((type & Rmulti) && ifc->m->addmulti != nil)
			(*ifc->m->addmulti)(ifc, a, lifc->local);
	} else {
		lp->ref++;
	}

	qunlock(&selftab);
}

/*
 *  These structures are unlinked from their chains while
 *  other threads may be using them.  To avoid excessive locking,
 *  just put them aside for a while before freeing them.
 *	called with &selftab locked
 */
static Iplink *freeiplink;
static Ipself *freeipself;

static void
iplinkfree(Iplink *p)
{
	Iplink **l, *np;
	ulong now = msec;

	l = &freeiplink;
	for(np = *l; np; np = *l){
		if(np->expire > now){
			*l = np->next;
			free(np);
			continue;
		}
		l = &np->next;
	}
	p->expire = now + 5000;		/* give other threads 5 secs to get out */
	p->next = nil;
	*l = p;
}
static void
ipselffree(Ipself *p)
{
	Ipself **l, *np;
	ulong now = msec;

	l = &freeipself;
	for(np = *l; np; np = *l){
		if(np->expire > now){
			*l = np->next;
			free(np);
			continue;
		}
		l = &np->next;
	}
	p->expire = now + 5000;		/* give other threads 5 secs to get out */
	p->next = nil;
	*l = p;
}

/*
 *  Decrement reference for this address on this link.
 *  Unlink from selftab if this is the last ref.
 *	called with c->car locked
 */
static void
remselfcache(Ipifc *ifc, Iplifc *lifc, uchar *a)
{
	Ipself *p, **l;
	Iplink *link, **l_self, **l_lifc;

	qlock(&selftab);

	/* find the unique selftab entry */
	l = &selftab.hash[hashipa(a)];
	for(p = *l; p; p = *l){
		if(ipcmp(p->a, a) == 0)
			break;
		l = &p->next;
	}

	if(p == nil)
		goto out;

	/*
	 *  walk down links from an ifc looking for one
	 *  that matches the selftab entry
	 */
	l_lifc = &lifc->link;
	for(link = *l_lifc; link; link = *l_lifc){
		if(link->self == p)
			break;
		l_lifc = &link->lifclink;
	}

	if(link == nil)
		goto out;

	/*
	 *  walk down the links from the selftab looking for
	 *  the one we just found
	 */
	l_self = &p->link;
	for(link = *l_self; link; link = *l_self){
		if(link == *(l_lifc))
			break;
		l_self = &link->selflink;
	}

	if(link == nil)
		panic("remselfcache");

	if(--(link->ref) != 0)
		goto out;

	if((p->type & Rmulti) && ifc->m->remmulti != nil)
		(*ifc->m->remmulti)(ifc, a, lifc->local);

	/* ref == 0, remove from both chains and free the link */
	*l_lifc = link->lifclink;
	*l_self = link->selflink;
	iplinkfree(link);

	/* remove from routing table */
	if(isv4(a))
		v4delroute(a+IPv4off, IPallbits+IPv4off);
	else
		v6delroute(a, IPallbits);
	
	if(p->link != nil)
		goto out;

	/* no more links, remove from hash and free */
	*l = p->next;
	ipselffree(p);

	/* if IPnoaddr, forget */
	if(ipcmp(a, v4prefix) == 0 || ipcmp(a, IPnoaddr) == 0)
		selftab.acceptall = 0;

out:
	qunlock(&selftab);
}

static void
dumpselftab(void)
{
	int i, count;
	Ipself *p;

	qlock(&selftab);
	for(i = 0; i < NHASH; i++){
		p = selftab.hash[i];
		if(p == nil)
			continue;
		count = 0;
		for(; p != nil && count++ < 6; p = p->next)
			print("(%i %d %lux)", p->a, p->type, p);
		print("\n");
	}
	qunlock(&selftab);
}


/*
 *  returns
 *	0		- no match
 *	Runi
 *	Rbcast
 *	Rmcast
 */
int
ipforme(uchar *addr)
{
	Ipself *p;
	int count;

	p = selftab.hash[hashipa(addr)];
	count = 0;
	for(; p; p = p->next){
		if(count++ > 1000){	/* check for loops */
			dumpselftab();
			break;
		}
		if(ipcmp(addr, p->a) == 0)
			return p->type;
	}

	/* hack to say accept anything */
	if(selftab.acceptall)
		return Runi;

	return 0;
}

/*
 *  find the ifc on same net as the remote system.  If none,
 *  return nil.
 */
Ipifc*
findipifc(uchar *remote, int type)
{
	Ipifc *ifc;
	Iplifc *lifc;
	Conv **cp;
	uchar gnet[IPaddrlen];

	for(cp = ipifc.conv; cp < &ipifc.conv[ipifc.nc]; cp++){
		if(*cp == 0)
			continue;
		ifc = (Ipifc*)(*cp)->ptcl;
		for(lifc = ifc->lifc; lifc; lifc = lifc->next){
			maskip(remote, lifc->mask, gnet);
			if(ipcmp(gnet, lifc->net) == 0){
				qunlock(&ipifc);
				return ifc;
			}
		}
	}

	/* for now for broadcast and mutlicast, just use first interface */
	if(type & (Rbcast|Rmulti)){
		for(cp = ipifc.conv; cp < &ipifc.conv[ipifc.nc]; cp++){
			if(*cp == 0)
				continue;
			ifc = (Ipifc*)(*cp)->ptcl;
			if(ifc->lifc != nil)
				return ifc;
		}
	}
		
	return nil;
}

/*
 *  find the local address 'closest' to the remote system, copy it to
 *  local and return the ifc for that address
 */
void
findlocalip(uchar *local, uchar *remote)
{
	Ipifc *ifc;
	Iplifc *lifc;
	Conv **cp;
	Route *r;
	uchar gate[IPaddrlen];
	uchar gnet[IPaddrlen];

	qlock(&ipifc);
	r = v6lookup(remote);
	
	if(r != nil){
		ifc = r->ifc;
		if(r->type & Rv4)
			v4tov6(gate, r->v4.gate);
		else
			ipmove(gate, r->v6.gate);

		if(r->type & Rifc){
			ipmove(local, gate);
			goto out;
		}

		/* find ifc address closest to the gateway to use */
		for(lifc = ifc->lifc; lifc; lifc = lifc->next){
			maskip(gate, lifc->mask, gnet);
			if(ipcmp(gnet, lifc->net) == 0){
				ipmove(local, lifc->local);
				goto out;
			}
		}
	}
		
	/* no match, choose first ifc local address */
	for(cp = ipifc.conv; cp < &ipifc.conv[ipifc.nc]; cp++){
		if(*cp == 0)
			continue;
		ifc = (Ipifc*)(*cp)->ptcl;
		for(lifc = ifc->lifc; lifc; lifc = lifc->next){
			ipmove(local, lifc->local);
			goto out;
		}
	}

out:
	qunlock(&ipifc);
}

/*
 *  return first v4 address associated with an interface
 */
int
ipv4local(Ipifc *ifc, uchar *addr)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc; lifc = lifc->next){
		if(isv4(lifc->local)){
			memmove(addr, lifc->local+IPv4off, IPv4addrlen);
			return 1;
		}
	}
	return 0;
}

/*
 *  return first v6 address associated with an interface
 */
int
ipv6local(Ipifc *ifc, uchar *addr)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc; lifc = lifc->next){
		if(!isv4(lifc->local)){
			ipmove(addr, lifc->local);
			return 1;
		}
	}
	return 0;
}

/*
 *  see if this address is bound to the interface
 */
Iplifc*
iplocalonifc(Ipifc *ifc, uchar *ip)
{
	Iplifc *lifc;

	for(lifc = ifc->lifc; lifc; lifc = lifc->next)
		if(ipcmp(ip, lifc->local) == 0)
			return lifc;
	return nil;
}


/*
 *  See if we're proxying for this address on this interface
 */
int
ipproxyifc(Ipifc *ifc, uchar *ip)
{
	Route *r;
	uchar net[IPaddrlen];
	Iplifc *lifc;

	/* see if this is a direct connected pt to pt address */
	r = v6lookup(ip);
	if(r == nil)
		return 0;
	if((r->type & Rifc) == 0)
		return 0;
	if((r->type & Rptpt) == 0)
		return 0;

	/* see if this is on the right interface */
	for(lifc = ifc->lifc; lifc; lifc = lifc->next){
		maskip(ip, lifc->mask, net);
		if(ipcmp(net, lifc->remote) == 0)
			return 1;
	}

	return 0;
}

/*
 *  return multicast version if any
 */
int
ipismulticast(uchar *ip)
{
	if(isv4(ip)){
		if(ip[IPv4off] >= 0xe0 && ip[IPv4off] < 0xf0)
			return V4;
	} else {
		if(ip[0] == 0xff)
			return V6;
	}
	return 0;
}

/*
 *  add a multicast address to an interface, called with c->car locked
 */
void
ipifcaddmulti(Conv *c, uchar *ma, uchar *ia)
{
	Ipifc *ifc;
	Iplifc *lifc;
	Conv **p;
	Ipmulti *multi, **l;
	
	for(l = &c->multi; *l; l = &(*l)->next)
		if(ipcmp(ma, (*l)->ma) == 0)
		if(ipcmp(ia, (*l)->ia) == 0)
			return;		/* it's already there */

	multi = *l = smalloc(sizeof(*multi));
	ipmove(multi->ma, ma);
	ipmove(multi->ia, ia);
	multi->next = nil;

	for(p = ipifc.conv; *p; p++){
		if((*p)->inuse == 0)
			continue;
		ifc = (Ipifc*)(*p)->ptcl;
		if(waserror()){
			wunlock(ifc);
			nexterror();
		}
		wlock(ifc);
		for(lifc = ifc->lifc; lifc; lifc = lifc->next)
			if(ipcmp(ia, lifc->local) == 0)
				addselfcache(ifc, lifc, ma, Rmulti);
		wunlock(ifc);
		poperror();
	}
}


/*
 *  remove a multicast address from an interface, called with c->car locked
 */
void
ipifcremmulti(Conv *c, uchar *ma, uchar *ia)
{
	Ipmulti *multi, **l;
	Iplifc *lifc;
	Conv **p;
	Ipifc *ifc;
	
	for(l = &c->multi; *l; l = &(*l)->next)
		if(ipcmp(ma, (*l)->ma) == 0)
		if(ipcmp(ia, (*l)->ia) == 0)
			break;

	multi = *l;
	if(multi == nil)
		return; 	/* we don't have it open */

	*l = multi->next;

	for(p = ipifc.conv; *p; p++){
		if((*p)->inuse == 0)
			continue;

		ifc = (Ipifc*)(*p)->ptcl;
		if(waserror()){
			wunlock(ifc);
			nexterror();
		}
		wlock(ifc);
		for(lifc = ifc->lifc; lifc; lifc = lifc->next)
			if(ipcmp(ia, lifc->local) == 0)
				remselfcache(ifc, lifc, ma);
		wunlock(ifc);
		poperror();
	}

	free(multi);
}

/*
 *  make lifc's join and leave multicast groups
 */
static char*
ipifcjoinmulti(Ipifc *ifc, char **argv, int argc)
{
	USED(ifc, argv, argc);
	return nil;
}

static char*
ipifcleavemulti(Ipifc *ifc, char **argv, int argc)
{
	USED(ifc, argv, argc);
	return nil;
}

