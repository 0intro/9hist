#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../ip/ip.h"

Fs fs;
Queue* qlog;

enum
{
	Qtopdir=	1,		/* top level directory */
	Qtopbase,
	Qarp=		Qtopbase,
	Qipifc,
	Qiproute,
	Qlog,

	Qprotodir,			/* directory for a protocol */
	Qprotobase,
	Qclone=		Qprotobase,

	Qconvdir,			/* directory for a conversation */
	Qconvbase,
	Qctl=		Qconvbase,
	Qdata,
	Qerr,
	Qlisten,
	Qlocal,
	Qremote,
	Qstatus,
};
#define TYPE(x) 	((x).path & 0x1f)
#define CONV(x) 	(((x).path >> 5)&0xfff)
#define PROTO(x) 	(((x).path >> 17)&0xff)
#define QID(p, c, y) 	(((p)<<17) | ((c)<<5) | (y))

static char network[] = "network";

static int
ip3gen(Chan *c, int i, Dir *dp)
{
	Qid q;
	Conv *cv;
	char *p;

	cv = fs.p[PROTO(c->qid)]->conv[CONV(c->qid)];
	switch(i) {
	default:
		return -1;
	case Qctl:
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qctl), 0};
		devdir(c, q, "ctl", 0, cv->owner, cv->perm, dp);
		return 1;
	case Qdata:
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qdata), 0};
		devdir(c, q, "data", qlen(cv->rq), cv->owner, cv->perm, dp);
		return 1;
	case Qerr:
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qerr), 0};
		devdir(c, q, "err", qlen(cv->eq), cv->owner, cv->perm, dp);
		return 1;
	case Qlisten:
		p = "listen";
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qlisten), 0};
		break;
	case Qlocal:
		p = "local";
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qlocal), 0};
		break;
	case Qremote:
		p = "remote";
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qremote), 0};
		break;
	case Qstatus:
		p = "status";
		q = (Qid){QID(PROTO(c->qid), CONV(c->qid), Qstatus), 0};
		break;
	}
	devdir(c, q, p, 0, cv->owner, 0444, dp);
	return 1;
}

static int
ip2gen(Chan *c, int i, Dir *dp)
{
	Qid q;

	switch(i) {
	case Qclone:
		q = (Qid){QID(PROTO(c->qid), 0, Qclone), 0};
		devdir(c, q, "clone", 0, network, 0444, dp);
		return 1;
	}	
	return -1;
}

static int
ip1gen(Chan *c, int i, Dir *dp)
{
	Qid q;
	char *p;

	switch(i) {
	default:
		return -1;
	case Qarp:
		p = "arp";
		q = (Qid){QID(0, 0, Qarp), 0};
		break;
	case Qipifc:
		p = "ipifc";
		q = (Qid){QID(0, 0, Qipifc), 0};
		break;
	case Qiproute:
		p = "iproute";
		q = (Qid){QID(0, 0, Qiproute), 0};
		break;
	case Qlog:
		p = "log";
		q = (Qid){QID(0, 0, Qlog), 0};
		break;
	}
	devdir(c, q, p, 0, network, 0444, dp);
	return 1;
}

static int
ipgen(Chan *c, Dirtab*, int, int s, Dir *dp)
{
	Qid q;
	Conv *cv;
	char name[16];

	switch(TYPE(c->qid)) {
	case Qtopdir:
		if(s < fs.np) {
			if(fs.p[s]->connect == nil)
				return 0;	/* protocol with no user interface */
			q = (Qid){QID(s, 0, Qprotodir)|CHDIR, 0};
			devdir(c, q, fs.p[s]->name, 0, network, CHDIR|0555, dp);
			return 1;
		}
		s -= fs.np;
		return ip1gen(c, s+Qtopbase, dp);
	case Qarp:
	case Qipifc:
	case Qlog:
	case Qiproute:
		return ip1gen(c, TYPE(c->qid), dp);
	case Qprotodir:
		if(s < fs.p[PROTO(c->qid)]->ac) {
			cv = fs.p[PROTO(c->qid)]->conv[s];
			sprint(name, "%d", s);
			q = (Qid){QID(PROTO(c->qid), s, Qconvdir)|CHDIR, 0};
			devdir(c, q, name, 0, cv->owner, CHDIR|0555, dp);
			return 1;
		}
		s -= fs.p[PROTO(c->qid)]->ac;
		return ip2gen(c, s+Qprotobase, dp);
	case Qclone:
		return ip2gen(c, TYPE(c->qid), dp);
	case Qconvdir:
		return ip3gen(c, s+Qconvbase, dp);
	case Qctl:
	case Qdata:
	case Qerr:
	case Qlisten:
	case Qlocal:
	case Qremote:
	case Qstatus:
		return ip3gen(c, TYPE(c->qid), dp);
	}
	return -1;
}

static void
ipreset(void)
{
}

static void
ipinit(void)
{
	int i;
	extern void (*ipprotoinit[])(Fs*);

	initfrag(100);
	for(i = 0; ipprotoinit[i]; i++)
		ipprotoinit[i](&fs);
	fmtinstall('i', eipconv);
	fmtinstall('I', eipconv);
	fmtinstall('E', eipconv);
}

static Chan*
ipattach(char* spec)
{
	Chan *c;

	c = devattach('I', spec);
	c->qid = (Qid){QID(0, 0, Qtopdir)|CHDIR, 0};

	return c;
}

static Chan*
ipclone(Chan* c, Chan* nc)
{
	return devclone(c, nc);
}

static int
ipwalk(Chan* c, char* name)
{
	Path *op;

	if(strcmp(name, "..") == 0){
		switch(TYPE(c->qid)){
		case Qtopdir:
		case Qprotodir:
			c->qid = (Qid){QID(0, 0, Qtopdir)|CHDIR, 0};
			break;
		case Qconvdir:
			c->qid = (Qid){QID(0, 0, Qprotodir)|CHDIR, 0};
			break;
		default:
			panic("ipwalk %lux", c->qid.path);
		}
		op = c->path;
		c->path = ptenter(&syspt, op, name);
		decref(op);
		return 1;
	}

	return devwalk(c, name, nil, 0, ipgen);
}

static void
ipstat(Chan* c, char* db)
{
	devstat(c, db, nil, 0, ipgen);
}

static int
incoming(void* arg)
{
	Conv *conv;

	conv = arg;
	return conv->incall != nil;
}

static int m2p[] = {
	[OREAD]		4,
	[OWRITE]	2,
	[ORDWR]		6
};

static Chan*
ipopen(Chan* c, int omode)
{
	Conv *cv, *nc;
	Proto *p;
	int perm;

	omode &= 3;
	perm = m2p[omode];

	switch(TYPE(c->qid)) {
	default:
		break;
	case Qipifc:
		c->aux = newifcconv();
		break;
	case Qlog:
		netlogopen();
		break;
	case Qtopdir:
	case Qprotodir:
	case Qconvdir:
	case Qstatus:
	case Qremote:
	case Qlocal:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qclone:
		p = fs.p[PROTO(c->qid)];
		cv = Fsprotoclone(p, up->user);
		if(cv == nil) {
			error(Enodev);
			break;
		}
		c->qid = (Qid){QID(p->x, cv->x, Qctl), 0};
		break;
	case Qdata:
	case Qctl:
	case Qerr:
		p = fs.p[PROTO(c->qid)];
		lock(p);
		cv = p->conv[CONV(c->qid)];
		lock(cv);
		if(waserror()) {
			unlock(cv);
			unlock(p);
			nexterror();
		}
		if((perm & (cv->perm>>6)) != perm) {
			if(strcmp(up->user, cv->owner) != 0)
				error(Eperm);
		 	if((perm & cv->perm) != perm)
				error(Eperm); 

		}
		cv->inuse++;
		if(cv->inuse == 1){
			memmove(cv->owner, up->user, NAMELEN);
			cv->perm = 0660;
		}
		unlock(cv);
		unlock(p);
		poperror();
		break;
	case Qlisten:
		cv = fs.p[PROTO(c->qid)]->conv[CONV(c->qid)];
		if(cv->state != Announced)
			error("not announced");

		nc = nil;
		while(nc == nil) {
			qlock(&cv->listenq);
			if(waserror()) {
				qunlock(&cv->listenq);
				nexterror();
			}

			sleep(&cv->listenr, incoming, cv);

			lock(cv);
			nc = cv->incall;
			if(nc != nil){
				cv->incall = nc->next;
				c->qid = (Qid){QID(PROTO(c->qid), nc->x, Qctl), 0};
				memmove(cv->owner, up->user, NAMELEN);
			}
			unlock(cv);

			qunlock(&cv->listenq);
			poperror();
		}
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
ipcreate(Chan*, char*, int, ulong)
{
	error(Eperm);
}

static void
ipremove(Chan*)
{
	error(Eperm);
}

static void
ipwstat(Chan*, char*)
{
	error(Eperm);
}

static void
closeconv(Conv *cv)
{
	Conv *nc;

	lock(cv);
	if(--cv->inuse > 0) {
		unlock(cv);
		return;
	}

	/* close all incoming calls since no listen will ever happen */
	for(nc = cv->incall; nc; nc = cv->incall){
		cv->incall = nc->next;
		closeconv(nc);
	}

	strcpy(cv->owner, network);
	cv->perm = 0660;

	/* The close routine will unlock the conv */
	cv->p->close(cv);
}

static void
ipclose(Chan* c)
{
	switch(TYPE(c->qid)) {
	default:
		break;
	case Qipifc:
		closeifcconv(c->aux);
		c->aux = nil;
		break;
	case Qlog:
		netlogclose();
		break;
	case Qdata:
	case Qctl:
	case Qerr:
		if(c->flag & COPEN)
			closeconv(fs.p[PROTO(c->qid)]->conv[CONV(c->qid)]);
	}
}

static long
ipread(Chan *ch, void *a, long n, ulong offset)
{
	Conv *c;
	Proto *x;
	byte ip[4];
	char buf[256], *p, *statename;

	p = a;
	switch(TYPE(ch->qid)) {
	default:
		error(Eperm);
	case Qtopdir:
	case Qprotodir:
	case Qconvdir:
		return devdirread(ch, a, n, 0, 0, ipgen);
	case Qarp:
		return arpread(a, offset, n);
	case Qipifc:
		return Mediaifcread(a, offset, n);
	case Qiproute:
		return routeread(a, offset, n);
	case Qlog:
		return netlogread(a, offset, n);
	case Qctl:
		sprint(buf, "%d", CONV(ch->qid));
		return readstr(offset, p, n, buf);
	case Qremote:
		c = fs.p[PROTO(ch->qid)]->conv[CONV(ch->qid)];
		hnputl(ip, c->raddr);
		sprint(buf, "%I!%d\n", ip, c->rport);
		return readstr(offset, p, n, buf);
	case Qlocal:
		c = fs.p[PROTO(ch->qid)]->conv[CONV(ch->qid)];
		if(media != nil && c->laddr == 0)
			hnputl(ip, Mediagetaddr(media));
		else
			hnputl(ip, c->laddr);
		sprint(buf, "%I!%d\n", ip, c->lport);
		return readstr(offset, p, n, buf);
	case Qstatus:
		x = fs.p[PROTO(ch->qid)];
		c = x->conv[CONV(ch->qid)];
		x->state(&statename, c);
		sprint(buf, "%s/%d %d %s \n", c->p->name, c->x, c->inuse, statename);
		return readstr(offset, p, n, buf);
	case Qdata:
		c = fs.p[PROTO(ch->qid)]->conv[CONV(ch->qid)];
		return qread(c->rq, a, n);
	case Qerr:
		c = fs.p[PROTO(ch->qid)]->conv[CONV(ch->qid)];
		return qread(c->eq, a, n);
	}
}

static Block*
ipbread(Chan* c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static void
setladdr(Conv* c)
{
	byte rem[4];

	hnputl(rem, c->raddr);
	c->laddr = Mediagetsrc(rem);
}

static void
setlport(Conv* c)
{
	Proto *p;
	ushort *pp;
	int x, found;

	p = c->p;
	if(c->restricted)
		pp = &p->nextrport;
	else
		pp = &p->nextport;
	lock(p);
	for(;;(*pp)++){
		/*
		 * Fsproto initialises p->nextport to 0 and the restricted
		 * ports (p->nextrport) to 600.
		 * Restricted ports must lie between 600 and 1024.
		 * For the initial condition or if the unrestricted port number
		 * has wrapped round, select a random port between 5000 and 1<<16
		 * to start at.
		 */
		if(c->restricted){
			if(*pp >= 1024)
				*pp = 600;
		}
		else while(*pp < 5000)
			*pp = nrand(1<<16);

		found = 0;
		for(x = 0; x < p->nc; x++){
			if(p->conv[x] == nil)
				break;
			if(p->conv[x]->lport == *pp){
				found = 1;
				break;
			}
		}
		if(!found)
			break;
	}
	c->lport = (*pp)++;
	unlock(p);
}

static void
setladdrport(Conv* c, char* str)
{
	char *p;
	uchar addr[Ipaddrlen];

	p = strchr(str, '!');
	if(p == nil) {
		p = str;
		c->laddr = 0;
	}
	else {
		*p++ = 0;
		parseip(addr, str);
		c->laddr = nhgetl(addr);
	}
	if(*p == '*')
		c->lport = 0;
	else {
		c->lport = atoi(p);
		if(c->lport == 0)
			setlport(c);
	}
}

static char*
setraddrport(Conv* c, char* str)
{
	char *p;
	uchar addr[Ipaddrlen];

	p = strchr(str, '!');
	if(p == nil)
		return "malformed address";
	*p++ = 0;
	parseip(addr, str);
	c->raddr = nhgetl(addr);
	c->rport = atoi(p);
	p = strchr(p, '!');
	if(p){
		if(strcmp(p, "!r") == 0)
			c->restricted = 1;
	}
	return nil;
}

static int
connected(void* a)
{
	return ((Conv*)a)->state == Connected;
}

static int
announced(void* a)
{
	return ((Conv*)a)->state == Announced;
}

static long
ipwrite(Chan* ch, char* a, long n, ulong)
{
	Conv *c;
	Proto *x;
	int nfield;
	char *p, *fields[10], buf[128];

	switch(TYPE(ch->qid)){
	default:
		error(Eperm);
	case Qarp:
		p = arpwrite(a, n);
		if(p != nil)
			error(p);
		return n;
	case Qipifc:
		p = Mediaifcwrite(ch->aux, a, n);
		if(p != nil)
			error(p);
		return n;
	case Qiproute:
		p = routewrite(a, n);
		if(p != nil)
			error(p);
		return n;
	case Qlog:
		p = netlogctl(a, n);
		if(p != nil)
			error(p);
		return n;
	case Qctl:
		x = fs.p[PROTO(ch->qid)];
		c = x->conv[CONV(ch->qid)];
		if(n > sizeof(buf)-1)
			n = sizeof(buf)-1;
		memmove(buf, a, n);
		buf[n] = '\0';
		nfield = parsefields(buf, fields, 10, " ");
		if(strcmp(fields[0], "connect") == 0){
			if(canqlock(&c->car) == 0)
				error("connect/announce in progress");
			if(waserror()) {
				qunlock(&c->car);
				nexterror();
			}
			c->state = Connecting;
			c->cerr[0] = '\0';
			p = x->connect(c, fields, nfield);
			if(p != nil)
				error(p);
			sleep(&c->cr, connected, c);
			if(c->cerr[0] != '\0')
				error(c->cerr);
			qunlock(&c->car);
			poperror();
			return n;
		}
		if(strcmp(fields[0], "announce") == 0){
			if(canqlock(&c->car) == 0)
				error("connect/announce in progress");
			if(waserror()) {
				qunlock(&c->car);
				nexterror();
			}
			c->state = Announcing;
			c->cerr[0] = '\0';
			p = x->announce(c, fields, nfield);
			if(p != nil)
				error(p);
			sleep(&c->cr, announced, c);
			if(c->cerr[0] != '\0')
				error(c->cerr);
			qunlock(&c->car);
			poperror();
			return n;
		}
		if(strcmp(fields[0], "bind") == 0){
			if(canqlock(&c->car) == 0)
				error("connect/announce in progress");
			if(waserror()) {
				qunlock(&c->car);
				nexterror();
			}
			switch(nfield){
			default:
				error("bad args to bind");
			case 2:
				setladdr(c);
				c->lport = atoi(fields[1]);
				if(c->lport == 0)
					setlport(c);
				break;
			}
			qunlock(&c->car);
			poperror();
			return n;
		}
		if(strcmp(fields[0], "ttl") == 0){
			if(nfield < 2)
				c->ttl = MAXTTL;
			else
				c->ttl = atoi(fields[1]);
			return n;
		}
		if(x->ctl != nil) {
			p = x->ctl(c, fields, nfield);
			if(p != nil)
				error(p);
			return n;
		}
		error("unknown control request");
	case Qdata:
		x = fs.p[PROTO(ch->qid)];
		c = x->conv[CONV(ch->qid)];

		qwrite(c->wq, a, n);
		x->kick(c, n);
	}
	return n;
}

static long
ipbwrite(Chan* c, Block* bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

Dev ipdevtab = {
	'I',
	"ip",

	ipreset,
	ipinit,
	ipattach,
	ipclone,
	ipwalk,
	ipstat,
	ipopen,
	ipcreate,
	ipclose,
	ipread,
	ipbread,
	ipwrite,
	ipbwrite,
	ipremove,
	ipwstat,
};

int
Fsproto(Fs *fs, Proto *p)
{
	if(fs->np >= Maxproto)
		return -1;

	if(p->ipproto > 0){
		if(fs->t2p[p->ipproto] != nil)
			return -1;
		fs->t2p[p->ipproto] = p;
	}

	p->qid.path = CHDIR|QID(fs->np, 0, Qprotodir);
	p->conv = malloc(sizeof(Conv*)*(p->nc+1));
	if(p->conv == nil)
		panic("Fsproto");

	p->x = fs->np;
	p->nextport = 0;
	p->nextrport = 600;
	fs->p[fs->np++] = p;
	return 0;
}

/*
 *  return true if this protocol is
 *  built in
 */
int
Fsbuiltinproto(Fs* fs, byte proto)
{
	return fs->t2p[proto] != nil;
}

Conv*
Fsprotoclone(Proto *p, char *user)
{
	char *junk;
	int unused;
	Conv *c, **pp, **ep;

	c = nil;
	lock(p);
	if(waserror()) {
		unlock(p);
		nexterror();
	}
	ep = &p->conv[p->nc];
	for(pp = p->conv; pp < ep; pp++) {
		c = *pp;
		if(c == nil){
			c = malloc(sizeof(Conv));
			if(c == nil)
				error(Enomem);
			lock(c);
			c->p = p;
			c->x = pp - p->conv;
			c->ptcl = malloc(p->ptclsize);
			if(c->ptcl == nil) {
				free(c);
				error(Enomem);
			}
			*pp = c;
			p->ac++;
			c->eq = qopen(1024, 1, 0, 0);
			(*p->create)(c);
			break;
		}
		if(canlock(c)){
			unused = p->state(&junk, c);
			if(c->inuse == 0 && unused)
				break;

			unlock(c);
		}
	}
	if(pp >= ep) {
		unlock(p);
		poperror();
		return nil;
	}

	c->inuse = 1;
	strcpy(c->owner, user);
	c->perm = 0660;
	c->state = 0;
	c->laddr = 0;
	c->raddr = 0;
	c->lport = 0;
	c->rport = 0;
	c->restricted = 0;
	c->ttl = MAXTTL;
	qreopen(c->rq);
	qreopen(c->wq);
	qreopen(c->eq);

	unlock(c);
	unlock(p);
	poperror();
	return c;
}

int
Fsconnected(Fs*, Conv* c, char* msg)
{
	if(msg != nil && *msg != '\0')
		strncpy(c->cerr, msg, ERRLEN-1);

	switch(c->state){

	case Announcing:
		c->state = Announced;
		break;

	case Connecting:
		c->state = Connected;
		break;
	}

	wakeup(&c->cr);
	return 0;
}

Proto*
Fsrcvpcol(Fs* fs, byte proto)
{
	return fs->t2p[proto];
}

Conv*
Fsnewcall(Fs*, Conv *c, Ipaddr raddr, ushort rport, Ipaddr laddr, ushort lport)
{
	Conv *nc;
	Conv **l;
	int i;

	lock(c);
	i = 0;
	for(l = &c->incall; *l; l = &(*l)->next)
		i++;
	if(i >= Maxincall) {
		unlock(c);
		return nil;
	}

	/* find a free conversation */
	nc = Fsprotoclone(c->p, network);
	if(nc == nil) {
		unlock(c);
		return nil;
	}
	nc->raddr = raddr;
	nc->rport = rport;
	nc->laddr = laddr;
	nc->lport = lport;
	nc->next = nil;
	*l = nc;
	unlock(c);

	wakeup(&c->listenr);

	return nc;
}

int
Fspcolstats(char *buf, int len)
{
	Proto **p;
	int n;

	n = 0;
	for(p = fs.p; *p; p++)
		n += snprint(buf + n, len - n,
			"%s: csum %d hlen %d len %d order %d rexmit %d\n",
			(*p)->name, (*p)->csumerr, (*p)->hlenerr, (*p)->lenerr,
			(*p)->order, (*p)->rexmit);
	return n;
}

char*
Fsstdconnect(Conv *c, char *argv[], int argc)
{
	char *p;

	switch(argc) {
	default:
		return "bad args to connect";
	case 2:
		p = setraddrport(c, argv[1]);
		if(p != nil)
			return p;
		setladdr(c);
		setlport(c);
		break;
	case 3:
		p = setraddrport(c, argv[1]);
		if(p != nil)
			return p;
		setladdr(c);
		c->lport = atoi(argv[2]);
		break;
	}
	return nil;
}

char*
Fsstdannounce(Conv* c, char* argv[], int argc)
{
	switch(argc){
	default:
		return "bad args to announce";
	case 2:
		setladdrport(c, argv[1]);
		break;
	}
	return nil;
}
