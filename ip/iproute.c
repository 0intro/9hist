#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"

enum
{
	Lroot	= 10,
};

Route *v4root[1<<Lroot], *v6root[1<<Lroot], *queue;

static void	walkadd(Route**, Route*);
static void	addnode(Route**, Route*);
static void	calcd(Route*);

Route*	v4freelist;
Route*	v6freelist;
RWlock	routelock;

static void
freeroute(Route *r)
{
	Route **l;

	r->left = nil;
	r->right = nil;
	if(r->type & Rv4)
		l = &v4freelist;
	else
		l = &v6freelist;
	r->mid = *l;
	*l = r;
}

static Route*
allocroute(int type)
{
	Route *r;
	int n;
	Route **l;

	if(type & Rv4){
		n = sizeof(RouteTree) + sizeof(V4route);
		l = &v4freelist;
	} else {
		n = sizeof(RouteTree) + sizeof(V6route);
		l = &v6freelist;
	}

	r = *l;
	if(r != nil){
		*l = r->mid;
	} else {
		r = malloc(n);
		if(r == nil)
			panic("out of routing nodes");
	}
	memset(r, 0, n);
	r->type = type;
	r->ifc = nil;

	return r;
}

static void
addqueue(Route **q, Route *r)
{
	Route *l;

	if(r == nil)
		return;

	l = allocroute(r->type);
	l->mid = *q;
	*q = l;
	l->left = r;
}

/*
 *   compare 2 v6 addresses
 */
static int
lcmp(ulong *a, ulong *b)
{
	int i;

	for(i = 0; i < IPllen; i++){
		if(a[i] > b[i])
			return 1;
		if(a[i] < b[i])
			return -1;
	}
	return 0;
}

/*
 *  compare 2 v4 or v6 ranges
 */
enum
{
	Rpreceeds,
	Rfollows,
	Requals,
	Rcontains,
	Rcontained,
};

static int
rangecompare(Route *a, Route *b)
{
	if(a->type & Rv4){
		if(a->v4.endaddress < b->v4.address)
			return Rpreceeds;

		if(a->v4.address > b->v4.endaddress)
			return Rfollows;

		if(a->v4.address <= b->v4.address
		&& a->v4.endaddress >= b->v4.endaddress){
			if(a->v4.address == b->v4.address
			&& a->v4.endaddress == b->v4.endaddress)
				return Requals;
			return Rcontains;
		}
		return Rcontained;
	}

	if(lcmp(a->v6.endaddress, b->v6.address) < 0)
		return Rpreceeds;

	if(lcmp(a->v6.address, b->v6.endaddress) > 0)
		return Rfollows;

	if(lcmp(a->v6.address, b->v6.address) <= 0
	&& lcmp(a->v6.endaddress, b->v6.endaddress) >= 0){
		if(lcmp(a->v6.address, b->v6.address) == 0
		&& lcmp(a->v6.endaddress, b->v6.endaddress) == 0)
				return Requals;
		return Rcontains;
	}

	return Rcontained;
}

static void
copygate(Route *old, Route *new)
{
	if(new->type & Rv4)
		memmove(old->v4.gate, new->v4.gate, IPv4addrlen);
	else
		memmove(old->v6.gate, new->v6.gate, IPaddrlen);
}

/*
 *  walk down a tree adding nodes back in
 */
static void
walkadd(Route **root, Route *p)
{
	Route *l, *r;

	l = p->left;
	r = p->right;
	p->left = 0;
	p->right = 0;
	addnode(root, p);
	if(l)
		walkadd(root, l);
	if(r)
		walkadd(root, r);
}

/*
 *  calculate depth
 */
static void
calcd(Route *p)
{
	Route *q;
	int d;

	if(p) {
		d = 0;
		q = p->left;
		if(q)
			d = q->depth;
		q = p->right;
		if(q && q->depth > d)
			d = q->depth;
		q = p->mid;
		if(q && q->depth > d)
			d = q->depth;
		p->depth = d+1;
	}
}

/*
 *  balance the tree at the current node
 */
static void
balancetree(Route **cur)
{
	Route *p, *l, *r;
	int dl, dr;

	/*
	 * if left and right are
	 * too out of balance,
	 * rotate tree node
	 */
	p = *cur;
	dl = 0; if(l = p->left) dl = l->depth;
	dr = 0; if(r = p->right) dr = r->depth;

	if(dl > dr+1) {
		p->left = l->right;
		l->right = p;
		*cur = l;
		calcd(p);
		calcd(l);
	} else
	if(dr > dl+1) {
		p->right = r->left;
		r->left = p;
		*cur = r;
		calcd(p);
		calcd(r);
	} else
		calcd(p);
}

/*
 *  add a new node to the tree
 */
static void
addnode(Route **cur, Route *new)
{
	Route *p;

	p = *cur;
	if(p == 0) {
		*cur = new;
		new->depth = 1;
		return;
	}

	switch(rangecompare(new, p)){
	case Rpreceeds:
		addnode(&p->left, new);
		break;
	case Rfollows:
		addnode(&p->right, new);
		break;
	case Rcontains:
		/*
		 *  if new node is superset
		 *  of tree node,
		 *  replace tree node and
		 *  queue tree node to be
		 *  merged into root.
		 */
		*cur = new;
		new->depth = 1;
		addqueue(&queue, p);
		break;
	case Requals:
		copygate(p, new);
		freeroute(new);
		break;
	case Rcontained:
		addnode(&p->mid, new);
		break;
	}
	
	balancetree(cur);
}

#define	V4H(a)	((a&0x07ffffff)>>(32-Lroot-5))

void
v4addroute(char *tag, uchar *a, uchar *mask, uchar *gate, int type)
{
	Route *p;
	ulong sa;
	ulong m;
	ulong ea;
	int h, eh;

	m = nhgetl(mask);
	sa = nhgetl(a) & m;
	ea = sa | ~m;

	eh = V4H(ea);
	for(h=V4H(sa); h<=eh; h++) {
		p = allocroute(Rv4 | type);
		p->v4.address = sa;
		p->v4.endaddress = ea;
		memmove(p->v4.gate, gate, sizeof(p->v4.gate));
		memmove(p->tag, tag, sizeof(p->tag));

		wlock(&routelock);
		addnode(&v4root[h], p);
		while(p = queue) {
			queue = p->mid;
			walkadd(&v4root[h], p->left);
			freeroute(p);
		}
		wunlock(&routelock);
	}

	ipifcaddroute(Rv4, a, mask, gate, type);
}

#define	V6H(a)	(((a)[IPllen-1] & 0x07ffffff)>>(32-Lroot-5))

void
v6addroute(char *tag, uchar *a, uchar *mask, uchar *gate, int type)
{
	Route *p;
	ulong sa[IPllen], ea[IPllen];
	ulong x, y;
	int h, eh;

	for(h = 0; h < IPllen; h++){
		x = nhgetl(a+4*h);
		y = nhgetl(mask+4*h);
		sa[h] = x & y;
		ea[h] = x | ~y;
	}

	eh = V6H(ea);
	for(h = V6H(sa); h <= eh; h++) {
		p = allocroute(type);
		memmove(p->v6.address, sa, IPaddrlen);
		memmove(p->v6.endaddress, ea, IPaddrlen);
		memmove(p->v6.gate, gate, IPaddrlen);
		memmove(p->tag, tag, sizeof(p->tag));

		wlock(&routelock);
		addnode(&v6root[h], p);
		while(p = queue) {
			queue = p->mid;
			walkadd(&v6root[h], p->left);
			freeroute(p);
		}
		wunlock(&routelock);
	}

	ipifcaddroute(0, a, mask, gate, type);
}

Route**
looknode(Route **cur, Route *r)
{
	Route *p;

	for(;;){
		p = *cur;
		if(p == 0)
			return 0;
	
		switch(rangecompare(r, p)){
		case Rcontains:
			return 0;
		case Rpreceeds:
			cur = &p->left;
			break;
		case Rfollows:
			cur = &p->right;
			break;
		case Rcontained:
			cur = &p->mid;
			break;
		case Requals:
			return cur;
		}
	}
}

void
v4delroute(uchar *a, uchar *mask)
{
	Route **r, *p;
	Route rt;
	int h, eh;
	ulong m;

	m = nhgetl(mask);
	rt.v4.address = nhgetl(a) & m;
	rt.v4.endaddress = rt.v4.address | ~m;
	rt.type = Rv4;

	eh = V4H(rt.v4.endaddress);
	for(h=V4H(rt.v4.address); h<=eh; h++) {
		wlock(&routelock);
		r = looknode(&v4root[h], &rt);
		if(r) {
			p = *r;
			*r = 0;
			addqueue(&queue, p->left);
			addqueue(&queue, p->mid);
			addqueue(&queue, p->right);
			freeroute(p);
			while(p = queue) {
				queue = p->mid;
				walkadd(&v4root[h], p->left);
				freeroute(p);
			}
		}
		wunlock(&routelock);
	}

	ipifcremroute(Rv4, a, mask);
}

void
v6delroute(uchar *a, uchar *mask)
{
	Route **r, *p;
	Route rt;
	int h, eh;
	ulong x, y;

	for(h = 0; h < IPllen; h++){
		x = nhgetl(a+4*h);
		y = nhgetl(mask+4*h);
		rt.v6.address[h] = x & y;
		rt.v6.endaddress[h] = x | ~y;
	}
	rt.type = 0;

	eh = V6H(rt.v6.endaddress);
	for(h=V6H(rt.v6.address); h<=eh; h++) {
		wlock(&routelock);
		r = looknode(&v6root[h], &rt);
		if(r) {
			p = *r;
			*r = 0;
			addqueue(&queue, p->left);
			addqueue(&queue, p->mid);
			addqueue(&queue, p->right);
			freeroute(p);
			while(p = queue) {
				queue = p->mid;
				walkadd(&v6root[h], p->left);
				freeroute(p);
			}
		}
		wunlock(&routelock);
	}

	ipifcremroute(0, a, mask);
}

Route*
v4lookup(uchar *a)
{
	Route *p, *q;
	ulong la;
	uchar gate[IPaddrlen];

	la = nhgetl(a);
	q = nil;
	for(p=v4root[V4H(la)]; p;)
		if(la >= p->v4.address) {
			if(la <= p->v4.endaddress) {
				q = p;
				p = p->mid;
			} else
				p = p->right;
		} else
			p = p->left;

	if(q && (q->ifc == nil || q->ifcid != q->ifc->ifcid)){
		v4tov6(gate, q->v4.gate);
		q->ifc = findipifc(gate, q->type);
		if(q->ifc == nil)
			return nil;
		q->ifcid = q->ifc->ifcid;
	}

	return q;
}

Route*
v6lookup(uchar *a)
{
	Route *p, *q;
	ulong la[IPllen];
	int h;
	ulong x, y;

	if(memcmp(a, v4prefix, 12) == 0){
		q = v4lookup(a+12);
		if(q != nil)
			return q;
	}

	for(h = 0; h < IPllen; h++)
		la[h] = nhgetl(a+4*h);

	q = 0;
	for(p=v6root[V6H(la)]; p;){
		for(h = 0; h < IPllen; h++){
			x = la[h];
			y = p->v6.address[h];
			if(x == y)
				continue;
			if(x < y){
				p = p->left;
				goto next;
			}
			break;
		}
		for(h = 0; h < IPllen; h++){
			x = la[h];
			y = p->v6.endaddress[h];
			if(x == y)
				continue;
			if(x > y){
				p = p->right;
				goto next;
			}
			break;
		}
		q = p;
		p = p->mid;
next:		;
	}

	if(q && q->ifc == nil){
		q->ifc = findipifc(q->v6.gate, q->type);
		if(q->ifc == nil)
			return nil;
		if(q->ifcid != q->ifc->ifcid)
			return nil;
	}
	
	return q;
}

enum
{
	Rlinelen=	89,
};

char *rformat = "%-24.24I %-24.24M %-24.24I %4.4s %4.4s %3s\n";

void
convroute(Route *r, uchar *addr, uchar *mask, uchar *gate, char *t, int *nifc)
{
	int i;
	char *p = t;

	if(r->type & Rv4){
		memmove(addr, v4prefix, IPv4off);
		hnputl(addr+IPv4off, r->v4.address);
		memset(mask, 0xff, IPv4off);
		hnputl(mask+IPv4off, ~(r->v4.endaddress ^ r->v4.address));
		memmove(gate, v4prefix, IPv4off);
		memmove(gate+IPv4off, r->v4.gate, IPv4addrlen);
	} else {
		for(i = 0; i < IPllen; i++){
			hnputl(addr + 4*i, r->v6.address[i]);
			hnputl(mask + 4*i, ~(r->v6.endaddress[i] ^ r->v6.address[i]));
		}
		memmove(gate, r->v6.gate, IPaddrlen);
	}

	memset(t, ' ', 4);
	t[4] = 0;
	if(r->type & Rv4)
		*p++ = '4';
	else
		*p++ = '6';
	if(r->type & Rifc)
		*p++ = 'i';
	if(r->type & Runi)
		*p++ = 'u';
	else if(r->type & Rbcast)
		*p++ = 'b';
	else if(r->type & Rmulti)
		*p++ = 'm';
	if(r->type & Rptpt)
		*p = 'p';

	if(r->ifc)
		*nifc = r->ifc->conv->x;
	else
		*nifc = -1;
}

/*
 *  this code is not in rr to reduce stack size
 */
static void
sprintroute(Route *r, Routewalk *rw)
{
	int nifc;
	char t[5], *iname, ifbuf[5];
	uchar addr[IPaddrlen], mask[IPaddrlen], gate[IPaddrlen];

	if(rw->o >= 0) {
		convroute(r, addr, mask, gate, t, &nifc);
		iname = "-";
		if(nifc != -1) {
			iname = ifbuf;
			sprint(ifbuf, "%d", nifc);
		}
		sprint(rw->p, rformat, addr, mask, gate, t, r->tag, iname);
		rw->p += Rlinelen;
	}
	rw->o++;
}

/*
 *  recurse descending tree, applying the function in Routewalk
 */
static int
rr(Route *r, Routewalk *rw)
{
	int h;

	if(rw->n <= rw->o)
		return 0;
	if(r == nil)
		return 1;

	if(rr(r->left, rw) == 0)
		return 0;

	if(r->type & Rv4)
		h = V4H(r->v4.address);
	else
		h = V6H(r->v6.address);

	if(h == rw->h)
		rw->walk(r, rw);

	if(rr(r->mid, rw) == 0)
		return 0;

	return rr(r->right, rw);
}

void
ipwalkroutes(Routewalk *rw)
{
	rlock(&routelock);
	if(rw->n > rw->o) {
		for(rw->h = 0; rw->h < nelem(v4root); rw->h++)
			if(rr(v4root[rw->h], rw) == 0)
				break;
	}
	if(rw->n > rw->o) {
		for(rw->h = 0; rw->h < nelem(v6root); rw->h++)
			if(rr(v6root[rw->h], rw) == 0)
				break;
	}
	runlock(&routelock);
}

long
routeread(char *p, ulong offset, int n)
{
	Routewalk rw;

	if(offset % Rlinelen)
		return 0;

	rw.p = p;
	rw.n = n/Rlinelen;
	rw.o = -(offset/Rlinelen);
	rw.walk = sprintroute;

	ipwalkroutes(&rw);

	if(rw.o < 0)
		rw.o = 0;

	return rw.o*Rlinelen;
}

/*
 *  this code is not in routeflush to reduce stack size
 */
void
delroute(Route *r)
{
	uchar addr[IPaddrlen];
	uchar mask[IPaddrlen];
	uchar gate[IPaddrlen];
	char t[5];
	int nifc;

	convroute(r, addr, mask, gate, t, &nifc);
	if(r->type & Rv4)
		v4delroute(addr+IPv4off, mask+IPv4off);
	else
		v6delroute(addr, mask);
}

/*
 *  recurse until one route is deleted
 *    returns 0 if nothing is deleted, 1 otherwise
 */
int
routeflush(Route *r)
{
	if(r == nil)
		return 0;
	if(routeflush(r->mid))
		return 1;
	if(routeflush(r->left))
		return 1;
	if(routeflush(r->right))
		return 1;
	if((r->type & Rifc) == 0){
		delroute(r);
		return 1;
	}
	return 0;
}

long
routewrite(Chan *c, char *p, int n)
{
	int h;
	char *tag;
	Cmdbuf *cb;
	uchar addr[IPaddrlen];
	uchar mask[IPaddrlen];
	uchar gate[IPaddrlen];

	cb = parsecmd(p, n);
	if(waserror()){
		free(cb);
		nexterror();
	}

	if(strcmp(cb->f[0], "flush") == 0){
		wlock(&routelock);
		for(h = 0; h < nelem(v4root); h++)
			while(routeflush(v4root[h]) == 1)
				;
		for(h = 0; h < nelem(v6root); h++)
			while(routeflush(v6root[h]) == 1)
				;
		wunlock(&routelock);
	} else if(strcmp(cb->f[0], "remove") == 0){
		if(cb->nf < 3)
			error(Ebadarg);
		parseip(addr, cb->f[1]);
		parseipmask(mask, cb->f[2]);
		if(memcmp(addr, v4prefix, IPv4off) == 0)
			v4delroute(addr+IPv4off, mask+IPv4off);
		else
			v6delroute(addr, mask);
	} else if(strcmp(cb->f[0], "add") == 0){
		if(cb->nf < 4)
			error(Ebadarg);
		parseip(addr, cb->f[1]);
		parseipmask(mask, cb->f[2]);
		parseip(gate, cb->f[3]);
		tag = "none";
		if(c != nil)
			tag = c->tag;
		if(memcmp(addr, v4prefix, IPv4off) == 0)
			v4addroute(tag, addr+IPv4off, mask+IPv4off, gate+IPv4off, 0);
		else
			v6addroute(tag, addr, mask, gate, 0);
	} else if(strcmp(cb->f[0], "tag") == 0) {
		if(cb->nf < 2)
			error(Ebadarg);

		h = strlen(cb->f[1]);
		if(h > sizeof(c->tag))
			h = sizeof(c->tag);
		strncpy(c->tag, cb->f[1], h);
		if(h < 4)
			memset(c->tag+h, ' ', sizeof(c->tag)-h);
	}

	poperror();
	free(cb);
	return n;
}
