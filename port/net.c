#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

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

	q.vers = 0;

	/* top level directory contains the name of the network */
	if(c->qid.path == CHDIR){
		switch(i){
		case 0:
			q.path = CHDIR | Q2nd;
			strcpy(buf, np->name);
			devdir(c, q, buf, 0, 0666, dp);
			break;
		default:
			return -1;
		}
		return 1;
	}

	/* second level contains clone plus all the conversations */
	if(STREAMID(c->qid.path) == 0){
		if(i == 0){
			q.path = Qclone;
			devdir(c, q, "clone", 0, 0666, dp);
		}else if(i < np->nconv){
			q.path = CHDIR|STREAMQID(i, Q3rd);
			sprint(buf, "%d", i);
			devdir(c, q, buf, 0, 0666, dp);
		}else
			return -1;
		return 1;
	}

	/* third level depends on the number of info files */
	switch(i){
	case 0:
		q.path = STREAMQID(STREAMID(c->qid.path), Sdataqid);
		devdir(c, q, "data", 0, 0666, dp);
		break;
	case 1:
		q.path = STREAMQID(STREAMID(c->qid.path), Sctlqid);
		devdir(c, q, "ctl", 0, 0666, dp);
		break;
	case 2:
		if(np->listen == 0)
			return 0;
		q.path = STREAMQID(STREAMID(c->qid.path), Qlisten);
		devdir(c, q, "listen", 0, 0666, dp);
		break;
	default:
		i -= 3;
		if(i >= np->ninfo)
			return -1;
		q.path = STREAMQID(STREAMID(c->qid.path), Qinf+i);
		devdir(c, q, np->info[i].name, 0, 0666, dp);
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
				devdir(c, c->qid, ".", 0L, CHDIR|0700, &dir);
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

Chan *
netopen(Chan *c, int omode, Network *np)
{
	int conv;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else {
		switch(STREAMTYPE(c->qid.path)){
		case Sdataqid:
		case Sctlqid:
			break;
		case Qlisten:
			conv = (*np->listen)(c);
			c->qid.path = STREAMQID(conv, Sctlqid);
			break;
		case Qclone:
			conv = (*np->clone)(c);
			c->qid.path = STREAMQID(conv, Sctlqid);
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
	return stringread(c, a, n, buf, offset);
}
