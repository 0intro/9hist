#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"fcall.h"

enum
{
	Qlisten=	1,
	Qclone=		2,
	Q2nd=		3,
	Q3rd=		4,
	Qinf=		5,
};

/*
 *  generate a 3 level directory
 */
int
netgen(Chan *c, void *vp, int ntab, int i, Dir *dp)
{
	Qid q;
	char buf[32];
	Network *np = vp;
	int t;
	int id;
	Netprot *p;
	int perm;
	char *o;

	q.vers = 0;

	/* top level directory contains the name of the network */
	if(c->qid.path == CHDIR){
		switch(i){
		case 0:
			q.path = CHDIR | Q2nd;
			strcpy(buf, np->name);
			devdir(c, q, buf, 0, eve, 0555, dp);
			break;
		default:
			return -1;
		}
		return 1;
	}

	/* second level contains clone plus all the conversations */
	t = STREAMTYPE(c->qid.path);
	if(t == Q2nd || t == Qclone){
		if(i == 0){
			q.path = Qclone;
			devdir(c, q, "clone", 0, eve, 0666, dp);
		}else if(i <= np->nconv){
			q.path = CHDIR|STREAMQID(i-1, Q3rd);
			sprint(buf, "%d", i-1);
			devdir(c, q, buf, 0, eve, 0555, dp);
		}else
			return -1;
		return 1;
	}

	/* third level depends on the number of info files */
	id = STREAMID(c->qid.path);
	p = &np->prot[id];
	if(*p->owner){
		o = p->owner;
		perm = p->mode;
	} else {
		o = eve;
		perm = 0666;
	}
	switch(i){
	case 0:
		q.path = STREAMQID(STREAMID(c->qid.path), Sdataqid);
		devdir(c, q, "data", 0, o, perm, dp);
		break;
	case 1:
		q.path = STREAMQID(STREAMID(c->qid.path), Sctlqid);
		devdir(c, q, "ctl", 0, o, perm, dp);
		break;
	case 2:
		if(np->listen == 0)
			return 0;
		q.path = STREAMQID(STREAMID(c->qid.path), Qlisten);
		devdir(c, q, "listen", 0, o, perm, dp);
		break;
	default:
		i -= 3;
		if(i >= np->ninfo)
			return -1;
		q.path = STREAMQID(STREAMID(c->qid.path), Qinf+i);
		devdir(c, q, np->info[i].name, 0, eve, 0444, dp);
		break;
	}
	return 1;
}

int	 
netwalk(Chan *c, char *name, Network *np)
{
	if(strcmp(name, "..") == 0) {
		switch(STREAMTYPE(c->qid.path)){
		case Q2nd:
			c->qid.path = CHDIR;
			break;
		case Q3rd:
			c->qid.path = CHDIR|Q2nd;
			break;
		default:
			panic("netwalk %lux", c->qid.path);
		}
		return 1;
	}

	return devwalk(c, name, (Dirtab*)np, 0, netgen);
}

void
netstat(Chan *c, char *db, Network *np)
{
	int i;
	Dir dir;

	for(i=0;; i++)
		switch(netgen(c, (Dirtab*)np, 0, i, &dir)){
		case -1:
			/*
			 * devices with interesting directories usually don't get
			 * here, which is good because we've lost the name by now.
			 */
			if(c->qid.path & CHDIR){
				devdir(c, c->qid, ".", 0L, eve, CHDIR|0555, &dir);
				convD2M(&dir, db);
				return;
			}
			print("netstat %c %lux\n", devchar[c->type], c->qid.path);
			error(Enonexist);
		case 0:
			break;
		case 1:
			if(eqqid(c->qid, dir.qid)){
				convD2M(&dir, db);
				return;
			}
			break;
		}
}

void
netwstat(Chan *c, char *db, Network *np)
{
	Dir dir;
	Netprot *p;

	p = &np->prot[STREAMID(c->qid.path)];
	lock(np);
	if(strncmp(p->owner, u->p->user, NAMELEN)){
		unlock(np);
		error(Eperm);
	}
	convM2D(db, &dir);
	strncpy(p->owner, dir.uid, NAMELEN);
	p->mode = dir.mode;
	unlock(np);
}

Chan *
netopen(Chan *c, int omode, Network *np)
{
	int id = 0;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		switch(STREAMTYPE(c->qid.path)){
		case Sdataqid:
		case Sctlqid:
			id = STREAMID(c->qid.path);
			break;
		case Qlisten:
			streamopen(c, np->devp);
			id = (*np->listen)(c);
			streamclose(c);
			c->qid.path = STREAMQID(id, Sctlqid);
			break;
		case Qclone:
			id = (*np->clone)(c);
			c->qid.path = STREAMQID(id, Sctlqid);
			break;
		default:
			if(omode != OREAD)
				error(Ebadarg);
		}
		switch(STREAMTYPE(c->qid.path)){
		case Sdataqid:
		case Sctlqid:
			streamopen(c, np->devp);
			if(np->protop && c->stream->devq->next->info != np->protop)
				pushq(c->stream, np->protop);
			if(netown(np, id, u->p->user, omode&7) < 0)
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
netread(Chan *c, void *a, long n, ulong offset, Network *np)
{
	int t;
	char buf[256];

	if(c->stream)
		return streamread(c, a, n);

	if(c->qid.path&CHDIR)
		return devdirread(c, a, n, (Dirtab*)np, 0, netgen);

	t = STREAMTYPE(c->qid.path);
	if(t < Qinf || t >= Qinf + np->ninfo)
		error(Ebadusefd);

	(*np->info[t-Qinf].fill)(c, buf, sizeof(buf));
	return stringread(a, n, buf, offset);
}

int
netown(Network *np, int id, char *o, int omode)
{
	static int access[] = { 0400, 0200, 0600, 0100 };
	Netprot *p;
	int mode;
	int t;

	p = &np->prot[id];
	lock(np);
	if(*p->owner){
		if(strncmp(o, p->owner, NAMELEN) == 0)	/* User */
			mode = p->mode;
		else if(strncmp(o, eve, NAMELEN) == 0)	/* Bootes is group */
			mode = p->mode<<3;
		else
			mode = p->mode<<6;		/* Other */

		t = access[omode&3];
		if((t & mode) == t){
			unlock(np);
			return 0;
		} else {
			unlock(np);
			return -1;
		}
	}
	strncpy(p->owner, o, NAMELEN);
	np->prot[id].mode = 0660;
	unlock(np);
	return 0;
}

void
netdisown(Network *np, int id)
{
	*np->prot[id].owner = 0;
}
