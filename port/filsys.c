#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#include	"fcall.h"

#define PRINT print

enum {
	Rreadhlen = 8,	/* number of chars in Rread message hdr */
};

typedef struct Ftrans Ftrans;

struct Ftrans{
	long	fid;
	Chan	*c;
	Chan	*pc;
};

static	Ftrans	*fsyshash;
static	Lock	fsyslock;

static	void	fsysnewchan(Chan *, long, Chan *);
static	Chan	*fsyschan(Chan *, long);
static	int	fsyserr(void);

void
filsysinit(void)
{
	fsyshash = ialloc(conf.nfsyschan * sizeof(Ftrans), 0);
}

void
filsys(Chan *protoc, char *msg, long msgn)
{
	Fcall r, t, *rf, *tf;
	Chan *c, *c0;
	long fid, n;

	if(!convM2S(msg, &r, msgn)){
		error(Ebadmsg);
	}
	rf = &r;
	tf = &t;
	if(waserror()){
		tf->type = Rerror;
		strncpy(tf->ename, u->error, sizeof(tf->ename));
		goto Errret;
	}
	fid = rf->fid;
	if(rf->type != Tattach)
		c = fsyschan(protoc, fid);
	switch(rf->type){
	case Tattach:
		c = clone(u->slash, 0);
		fsysnewchan(protoc, fid, c);
		break;
	case Tclone:
		c0 = clone(c, 0);
		c0 = domount(c0);
		fsysnewchan(protoc, rf->newfid, c0);
		break;
	case Twalk:
		if((c0 = walk(c, rf->name, 1)) == 0)
			error(Enonexist);
		if(c != c0)
			fsysnewchan(protoc, fid, c0);
		tf->qid = c0->qid;
		break;
	case Topen:
		c0 = (*devtab[c->type].open)(c, rf->mode);
		if(c != c0)
			fsysnewchan(protoc, fid, c0);
		if(rf->mode & OCEXEC)
			c0->flag |= CCEXEC;
		break;
	case Tcreate:
		if((c->flag&(CMOUNT|CCREATE)) == CMOUNT)
			c0 = createdir(c);
		(*devtab[c->type].create)(c, rf->name, rf->mode, rf->perm);
		if(rf->mode & OCEXEC)
			c->flag |= CCEXEC;
		break;
	case Tread:
		qlock(c);
		if(waserror()){
			qunlock(c);
			nexterror();
		}
		n = rf->count;
		tf->count = 0;
		/* overwrite the input msg -- assumes buffer is big enough */
		tf->data = msg+Rreadhlen;
		if(c->qid.path&CHDIR){
			n -= n%DIRLEN;
			if(c->offset%DIRLEN || n==0)
				error(Ebaddirread);
		}
		if((c->qid.path&CHDIR) && (c->flag&CMOUNT))
			n = unionread(c, tf->data, n);
		else
			n = (*devtab[c->type].read)(c, tf->data, n);
		c->offset += n;
		qunlock(c);
		poperror();
		tf->count = n;
		break;
	case Twrite:
		qlock(c);
		if(waserror()){
			qunlock(c);
			nexterror();
		}
		if(c->qid.path & CHDIR)
			error(Eisdir);
		n = rf->count;
		tf->count = 0;
		n = (*devtab[c->type].write)(c, rf->data, n);
		c->offset += n;
		qunlock(c);
		poperror();
		tf->count = n;
		break;
	case Tclunk:
		close(c);
		fsysnewchan(protoc, rf->fid, 0);
		break;
	case Tremove:
		(*devtab[c->type].remove)(c);
		break;
	case Tstat:
		(*devtab[c->type].stat)(c, tf->stat);
		break;
	case Twstat:
		(*devtab[c->type].wstat)(c, rf->stat);
		break;
	case Tflush:
		/* really, this message has to be handled by caller */
		break;
	default:
		error(Ebadmsg);
	}
	poperror();
	tf->type = rf->type+1;
   Errret:
	tf->fid = rf->fid;
	tf->tag = rf->tag;
	/* assume can reuse msg buf */
	n = convS2M(tf, msg);
	qlock(protoc);
	if(waserror()){
		qunlock(protoc);
		nexterror();
	}
	(*devtab[protoc->type].write)(protoc, msg, n);
	qunlock(protoc);
	poperror();
}

/*
 * hash table discipline: linear probe
 *    a fid of 0 means 'free'; a fid of -1 means 'deleted'
 *    use a function as per Knuth
 *    only a function of fid, but protoc checked for match
 */
static int
fhash(long fid)
{
	long x;

	x = fid*29128;
	if(x < 0)
		x = -x;
	x %= 262143;
	return x % conf.nfsyschan;
}

/*
 * Register channel c for fid (if c!= 0).
 * If c==0, unregister.
 * The protoc argument is used to disambiguate
 * identical fids from different attaches.
 */
static void
fsysnewchan(Chan *protoc, long fid, Chan *c)
{
	int i, m;
	Ftrans *f, *fstop, *fend;

	i = fhash(fid);
	f = fsyshash+i;
	m = conf.nfsyschan;
	fstop = f+(i+m-1)%m;
	fend = fsyshash+m;
	lock(&fsyslock);
	while(f != fstop){
		if(f->fid == fid && f->pc == protoc){
			if(c)
				f->c = c;
			else
				f->fid = -1;
			break;
		}
		if(f->fid <= 0){
			f->c = c;
			f->fid = fid;
			f->pc = protoc;
			break;
		}
		if(++f == fend)
			f = fsyshash;	
	}
	unlock(&fsyslock);
	if(f == fstop)
		error(Enofilsys);
}

/* Return the channel registered for fid */
static Chan*
fsyschan(Chan *protoc, long fid)
{
	int i, m;
	Ftrans *f, *fstop, *fend;

	i = fhash(fid);
	f = fsyshash+i;
	if(f->fid == fid && f->pc == protoc)
		return f->c;
	m = conf.nfsyschan;
	fstop = f;
	if(++f == fend)
		f = fsyshash;
	fend = fsyshash+m;
	while(f != fstop){
		if(f->fid == fid && f->pc == protoc)
			return f->c;
		if(f->fid == 0)
			break;
		if(++f == fend)
			f = fsyshash;	
	}
	error(Egreg);
}

