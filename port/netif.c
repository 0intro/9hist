#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/netif.h"

uchar etherbcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int netown(Netfile*, char*, int);
static int openfile(Netif*, int);
static char* matchtoken(char*, char*);

/*
 *  set up a new network interface
 */
void
netifinit(Netif *nif, char *name, int nfile, ulong limit)
{
	nif->name = name;
	nif->nfile = nfile;
	nif->f = xalloc(nfile*sizeof(Netfile*));
	memset(nif->f, 0, nfile*sizeof(Netfile*));
	nif->limit = limit;
	nif->out = qopen(limit, 1, 0, 0);
}

/*
 *  generate a 3 level directory
 */
static int
netifgen(Chan *c, void *vp, int ntab, int i, Dir *dp)
{
	Qid q;
	char buf[32];
	Netif *nif = vp;
	Netfile *f;
	int t;
	int perm;
	char *o;

	USED(ntab);
	q.vers = 0;

	/* top level directory contains the name of the network */
	if(c->qid.path == CHDIR){
		switch(i){
		case 0:
			q.path = CHDIR | N2ndqid;
			strcpy(buf, nif->name);
			devdir(c, q, buf, 0, eve, 0555, dp);
			break;
		default:
			return -1;
		}
		return 1;
	}

	/* second level contains clone plus all the conversations */
	t = NETTYPE(c->qid.path);
	if(t == N2ndqid || t == Ncloneqid){
		if(i == 0){
			q.path = Ncloneqid;
			devdir(c, q, "clone", 0, eve, 0666, dp);
		}else if(i <= nif->nfile){
			if(nif->f[i-1] == 0)
				return 0;
			q.path = CHDIR|NETQID(i-1, N3rdqid);
			sprint(buf, "%d", i-1);
			devdir(c, q, buf, 0, eve, 0555, dp);
		}else
			return -1;
		return 1;
	}

	/* third level */
	f = nif->f[NETID(c->qid.path)];
	if(f == 0)
		return 0;
	if(*f->owner){
		o = f->owner;
		perm = f->mode;
	} else {
		o = eve;
		perm = 0666;
	}
	switch(i){
	case 0:
		q.path = NETQID(NETID(c->qid.path), Ndataqid);
		devdir(c, q, "data", 0, o, perm, dp);
		break;
	case 1:
		q.path = NETQID(NETID(c->qid.path), Nctlqid);
		devdir(c, q, "ctl", 0, o, perm, dp);
		break;
	case 2:
		q.path = NETQID(NETID(c->qid.path), Nstatqid);
		devdir(c, q, "stats", 0, eve, 0444, dp);
		break;
	case 3:
		q.path = NETQID(NETID(c->qid.path), Ntypeqid);
		devdir(c, q, "type", 0, eve, 0444, dp);
		break;
	default:
		return -1;
	}
	return 1;
}

int
netifwalk(Netif *nif, Chan *c, char *name)
{
	return devwalk(c, name, (Dirtab *)nif, 0, netifgen);
}

Chan*
netifopen(Netif *nif, Chan *c, int omode)
{
	int id = 0;
	Netfile *f;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		switch(NETTYPE(c->qid.path)){
		case Ndataqid:
		case Nctlqid:
			id = NETID(c->qid.path);
			openfile(nif, id);
			break;
		case Ncloneqid:
			id = openfile(nif, -1);
			c->qid.path = NETQID(id, Nctlqid);
			ptclone(c, 0, id);
			break;
		default:
			if(omode != OREAD)
				error(Ebadarg);
		}
		switch(NETTYPE(c->qid.path)){
		case Ndataqid:
		case Nctlqid:
			f = nif->f[id];
			if(netown(f, up->user, omode&7) < 0)
				error(Eperm);
			break;
		}
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

long
netifread(Netif *nif, Chan *c, void *a, long n, ulong offset)
{
	int i, j;
	Netfile *f;
	char buf[256];

	if(c->qid.path&CHDIR)
		return devdirread(c, a, n, (Dirtab*)nif, 0, netifgen);

	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		f = nif->f[NETID(c->qid.path)];
		return qread(f->in, a, n);
	case Nctlqid:
		return readnum(offset, a, n, NETID(c->qid.path), NUMSIZE);
	case Nstatqid:
		j = sprint(buf, "in: %d\n", nif->inpackets);
		j += sprint(buf+j, "out: %d\n", nif->outpackets);
		j += sprint(buf+j, "crc errs: %d\n", nif->crcs);
		j += sprint(buf+j, "overflows: %d\n", nif->overflows);
		j += sprint(buf+j, "framing errs: %d\n", nif->frames);
		j += sprint(buf+j, "buffer errs: %d\n", nif->buffs);
		j += sprint(buf+j, "output errs: %d\n", nif->oerrs);
		j += sprint(buf+j, "addr: ");
		for(i = 0; i < nif->alen; i++)
			j += sprint(buf+j, "%2.2ux", nif->addr[i]);
		sprint(buf+j, "\n");
		return readstr(offset, a, n, buf);
	case Ntypeqid:
		f = nif->f[NETID(c->qid.path)];
		return readnum(offset, a, n, f->type, NUMSIZE);
	}
	error(Ebadarg);
	return -1;	/* not reached */
}

/*
 *  the devxxx.c that calls us handles writing data, it knows best
 */
long
netifwrite(Netif *nif, Chan *c, void *a, long n)
{
	Netfile *f;
	char *p;
	char buf[256];

	if(NETTYPE(c->qid.path) != Nctlqid)
		error(Eperm);

	if(n >= sizeof(buf))
		n = sizeof(buf)-1;
	memmove(buf, a, n);
	buf[n] = 0;

	qlock(nif);
	f = nif->f[NETID(c->qid.path)];
	if(p = matchtoken(buf, "connect")){
		f->type = atoi(p);
		if(f->type < 0)
			nif->all++;
	} else if(matchtoken(buf, "promiscuous")){
		f->prom = 1;
		nif->prom++;
		if(nif->prom == 1)
			(*nif->promiscuous)(nif->arg, 1);
	}
	qunlock(nif);
	return n;
}

void
netifwstat(Netif *nif, Chan *c, char *db)
{
	Dir dir;
	Netfile *f;

	f = nif->f[NETID(c->qid.path)];
	if(f == 0)
		error(Enonexist);

	if(netown(f, up->user, OWRITE) < 0)
		error(Eperm);

	convM2D(db, &dir);
	strncpy(f->owner, dir.uid, NAMELEN);
	f->mode = dir.mode;
}

void
netifstat(Netif *nif, Chan *c, char *db)
{
	devstat(c, db, (Dirtab *)nif, 0, netifgen);
}

void
netifclose(Netif *nif, Chan *c)
{
	Netfile *f;
	int t;

	if((c->flag & COPEN) == 0)
		return;

	t = NETTYPE(c->qid.path);
	if(t != Ndataqid && t != Nctlqid)
		return;

	f = nif->f[NETID(c->qid.path)];
	qlock(f);
	if(--(f->inuse) == 0){
		if(f->prom){
			qlock(nif);
			if(--(nif->prom) == 0)
				(*nif->promiscuous)(nif->arg, 0);
			qunlock(nif);
		}
		if(f->type < 0){
			qlock(nif);
			--(nif->all);
			qunlock(nif);
		}
		f->owner[0] = 0;
		f->type = 0;
		qclose(f->in);
	}
	qunlock(f);
}

Lock netlock;

static int
netown(Netfile *p, char *o, int omode)
{
	static int access[] = { 0400, 0200, 0600, 0100 };
	int mode;
	int t;

	lock(&netlock);
	if(*p->owner){
		if(strncmp(o, p->owner, NAMELEN) == 0)	/* User */
			mode = p->mode;
		else if(strncmp(o, eve, NAMELEN) == 0)	/* Bootes is group */
			mode = p->mode<<3;
		else
			mode = p->mode<<6;		/* Other */

		t = access[omode&3];
		if((t & mode) == t){
			unlock(&netlock);
			return 0;
		} else {
			unlock(&netlock);
			return -1;
		}
	}
	strncpy(p->owner, o, NAMELEN);
	p->mode = 0660;
	unlock(&netlock);
	return 0;
}

/*
 *  Increment the reference count of a network device.
 *  If id < 0, return an unused ether device.
 */
static int
openfile(Netif *nif, int id)
{
	Netfile *f, **fp, **efp;

	if(id >= 0){
		f = nif->f[id];
		if(f == 0)
			error(Enodev);
		qlock(f);
		qreopen(f->in);
		f->inuse++;
		qunlock(f);
		return id;
	}

	qlock(nif);
	efp = &nif->f[nif->nfile];
	for(fp = nif->f; fp < efp; fp++){
		f = *fp;
		if(f == 0){
			f = malloc(sizeof(Netfile));
			if(f == 0){
				qunlock(nif);
				error(Enodev);
			}
			*fp = f;
			f->in = qopen(nif->limit, 1, 0, 0);
			qlock(f);
		} else {
			qlock(f);
			if(f->inuse){
				qunlock(f);
				continue;
			}
		}
		f->inuse = 1;
		qreopen(f->in);
		netown(f, up->user, 0);
		qunlock(f);
		qunlock(nif);
		return fp - nif->f;
	}
	qunlock(nif);
	error(Enodev);
	return -1;	/* not reached */
}

/*
 *  look for a token starting a string,
 *  return a pointer to first non-space char after it
 */
static char*
matchtoken(char *p, char *token)
{
	int n;

	n = strlen(token);
	if(strncmp(p, token, n))
		return 0;
	p += n;
	if(*p == 0)
		return p;
	if(*p != ' ' && *p != '\t' && *p != '\n')
		return 0;
	while(*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return p;
}
