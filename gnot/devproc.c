#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

enum{
	Qdir,
	Qctl,
	Qmem,
	Qnote,
	Qnotepg,
	Qproc,
	Qstatus,
	Qtext,
};

Dirtab procdir[]={
	"ctl",		Qctl,		0,			0600,
	"mem",		Qmem,		0,			0600,
	"note",		Qnote,		0,			0600,
	"notepg",	Qnotepg,	0,			0200,
	"proc",		Qproc,		sizeof(Proc),		0600,
	"status",	Qstatus,	NAMELEN+12+6*12,	0600,
	"text",		Qtext,		0,			0600,
};

/*
 * Qids are, from bottom to top:
 *	 4 bits of file type (qids above)
 *	12 bits of process slot number + 1
 *	15 bits of pid, for consistency checking
 */
#define	NPROC	(sizeof procdir/sizeof(Dirtab))
#define	QSHIFT	4	/* location in qid of proc slot # */
#define	PIDSHIFT 16	/* location in qid of pid */
#define	PIDMASK	0x7FFF	/* low bits of pid used in qid */
#define	QID(q)	(((q)&0x0000000F)>>0)
#define	SLOT(q)	((((q)&0x0000FFF0)>>QSHIFT)-1)
#define	PID(q)	(((q)&0x7FFF0000)>>PIDSHIFT)

int
procgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Proc *p;
	char buf[NAMELEN];
	ulong pid;

	if(c->qid == CHDIR){
		if(s >= conf.nproc)
			return -1;
		p = proctab(s);
		pid = p->pid;
		if(pid == 0)
			return 0;
		sprint(buf, "%d", pid);
		devdir(c, CHDIR|(pid<<PIDSHIFT)|((s+1)<<QSHIFT), buf, 0, CHDIR|0500, dp);
		return 1;
	}
	if(s >= NPROC)
		return -1;
	if(tab)
		panic("procgen");
	tab = &procdir[s];
	devdir(c, (~CHDIR)&(c->qid|tab->qid), tab->name, tab->length, tab->perm, dp);
	return 1;
}

void
procinit(void)
{
	if(conf.nproc >= (1<<(16-QSHIFT))-1)
		print("warning: too many procs for devproc\n");
}

void
procreset(void)
{
}

Chan*
procattach(char *spec)
{
	Chan *c;
	return devattach('p', spec);
}

Chan*
procclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
procwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, procgen);
}

void
procstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, procgen);
}

Chan *
procopen(Chan *c, int omode)
{
	Proc *p;
	Pgrp *pg;
	Orig *o;
	Chan *tc;

	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eperm);
		goto done;
	}
	p = proctab(SLOT(c->qid));
	pg = p->pgrp;
	if((p->pid&PIDMASK) != PID(c->qid))
    Died:
		error(0, Eprocdied);
	omode = openmode(omode);

	switch(QID(c->qid)){
	case Qtext:
		o = p->seg[TSEG].o;
		if(o==0 || p->state==Dead)
			goto Died;
		tc = o->chan;
		if(tc == 0)
			goto Died;
		if(incref(tc) == 0){
    Close:
			close(tc);
			goto Died;
		}
		if(!(tc->flag&COPEN) || tc->mode!=OREAD)
			goto Close;
		if((p->pid&PIDMASK) != PID(c->qid))
			goto Close;
		qlock(tc);
		tc->offset = 0;
		qunlock(tc);
		return tc;
	case Qctl:
	case Qnote:
		break;

	case Qnotepg:
		if(omode!=OWRITE || pg->pgrpid==1)	/* easy to do by mistake */
			error(0, Eperm);
		c->pgrpid = (pg->pgrpid<<PIDSHIFT)|((pg->index+1)<<QSHIFT);
		break;

	case Qdir:
	case Qmem:
	case Qproc:
	case Qstatus:
		if(omode != OREAD)
			error(0, Eperm);
		break;
	default:
		pprint("unknown qid in devopen\n");
		error(0, Egreg);
	}
	/*
	 * Affix pid to qid
	 */
	if(p->state != Dead)
		c->qid |= (p->pid&PIDMASK)<<PIDSHIFT;
   done:
	c->mode = omode;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
proccreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void
procremove(Chan *c)
{
	error(0, Eperm);
}

void
procwstat(Chan *c, char *db)
{
	error(0, Eperm);
}

void
procclose(Chan * c)
{
}

long
procread(Chan *c, void *va, long n)
{
	char *a = va, *b;
	char statbuf[2*NAMELEN+12+6*12];
	Proc *p;
	Seg *s;
	Orig *o;
	Page *pg;
	KMap *k;
	PTE *pte, *opte;
	int i;
	long l;
	long pid;
	User *up;

	if(c->qid & CHDIR)
		return devdirread(c, a, n, 0, 0, procgen);

	/*
	 * BUG: should lock(&p->debug)?
	 */
	p = proctab(SLOT(c->qid));
	if((p->pid&PIDMASK) != PID(c->qid))
		error(0, Eprocdied);

	switch(QID(c->qid)){
	case Qmem:
		/*
		 * One page at a time
		 */
		if(((c->offset+n)&~(BY2PG-1)) != (c->offset&~(BY2PG-1)))
			n = BY2PG - (c->offset&(BY2PG-1));
		s = seg(p, c->offset);
		if(s){
			o = s->o;
			if(o == 0)
				error(0, Eprocdied);
			lock(o);
			if(s->o!=o || (p->pid&PIDMASK)!=PID(c->qid)){
				unlock(o);
				error(0, Eprocdied);
			}
			if(seg(p, c->offset) != s){
				unlock(o);
				error(0, Egreg);
			}
			pte = &o->pte[(c->offset-o->va)>>PGSHIFT];
			if(s->mod){
				opte = pte;
				while(pte = pte->nextmod)	/* assign = */
					if(pte->proc == p)
						break;
				if(pte == 0)
					pte = opte;
			}
			pg = pte->page;
			unlock(o);
			if(pg == 0){
				pprint("nonresident page addr %lux (complain to rob)\n", c->offset);
				memset(a, 0, n);
			}else{
				k = kmap(pg);
				b = (char*)VA(k);
				memcpy(a, b+(c->offset&(BY2PG-1)), n);
				kunmap(k);
			}
			return n;
		}

		/* u area */
		if(c->offset>=USERADDR && c->offset<USERADDR+BY2PG){
			if(c->offset+n > USERADDR+BY2PG)
				n = USERADDR+BY2PG - c->offset;
			pg = p->upage;
			if(pg==0 || (p->pid&PIDMASK)!=PID(c->qid))
				error(0, Eprocdied);
			k = kmap(pg);
			b = (char*)VA(k);
			memcpy(a, b+(c->offset-USERADDR), n);
			kunmap(k);
			return n;
		}

		/* kernel memory.  BUG: shouldn't be so easygoing. BUG: mem mapping? */
		if(c->offset>=KZERO && c->offset<KZERO+conf.npage0*BY2PG){
			if(c->offset+n > KZERO+conf.npage0*BY2PG)
				n = KZERO+conf.npage0*BY2PG - c->offset;
			memcpy(a, (char*)c->offset, n);
			return n;
		}
		return 0;
		break;

	case Qnote:
		lock(&p->debug);
		if(waserror()){
			unlock(&p->debug);
			nexterror();
		}
		if((p->pid&PIDMASK) != PID(c->qid))
			error(0, Eprocdied);
		k = kmap(p->upage);
		up = (User*)VA(k);
		if(up->p != p){
			kunmap(k);
			pprint("note read u/p mismatch");
			error(0, Egreg);
		}
		if(n < ERRLEN)
			error(0, Etoosmall);
		if(up->nnote == 0)
			n = 0;
		else{
			memcpy(va, up->note[0].msg, ERRLEN);
			up->nnote--;
			memcpy(&up->note[0], &up->note[1], up->nnote*sizeof(Note));
			n = ERRLEN;
		}
		kunmap(k);
		unlock(&p->debug);
		return n;

	case Qproc:
		if(c->offset >= sizeof(Proc))
			return 0;
		if(c->offset+n > sizeof(Proc))
			n = sizeof(Proc) - c->offset;
		memcpy(a, ((char*)p)+c->offset, n);
		return n;

	case Qstatus:
		if(c->offset >= sizeof statbuf)
			return 0;
		if(c->offset+n > sizeof statbuf)
			n = sizeof statbuf - c->offset;
		sprint(statbuf, "%-27s %-27s %-11s ", p->text, p->pgrp->user, statename[p->state]);
		for(i=0; i<6; i++){
			l = p->time[i];
			if(i == TReal)
				l = MACHP(0)->ticks - l;
			l = TK2MS(l);
			readnum(0, statbuf+2*NAMELEN+12+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		memcpy(a, statbuf+c->offset, n);
		return n;
	}
	error(0, Egreg);
}


long
procwrite(Chan *c, void *va, long n)
{
	Proc *p;
	Pgrp *pg;
	User *up;
	KMap *k;
	char buf[ERRLEN];

	if(c->qid & CHDIR)
		error(0, Eisdir);

	/*
	 * Special case: don't worry about process, just use remembered group
	 */
	if(QID(c->qid) == Qnotepg){
		pg = pgrptab(SLOT(c->pgrpid));
		lock(&pg->debug);
		if(waserror()){
			unlock(&pg->debug);
			nexterror();
		}
		if((pg->pgrpid&PIDMASK) != PID(c->pgrpid)){
			unlock(&pg->debug);
  	  		goto Died;
		}
		pgrpnote(pg, va, n, NUser);
		unlock(&pg->debug);
		return n;
	}

	p = proctab(SLOT(c->qid));
	lock(&p->debug);
	if(waserror()){
		unlock(&p->debug);
		nexterror();
	}
	if((p->pid&PIDMASK) != PID(c->qid))
    Died:
		error(0, Eprocdied);

	switch(QID(c->qid)){
	case Qctl:
		if(p->state==Broken && n>=4 && strncmp(va, "exit", 4)==0)
			ready(p);
		else
			error(0, Ebadctl);
		break;
	case Qnote:
		k = kmap(p->upage);
		up = (User*)VA(k);
		if(up->p != p){
			kunmap(k);
			pprint("note write u/p mismatch");
			error(0, Egreg);
		}
		kunmap(k);
		if(n >= ERRLEN-1)
			error(0, Etoobig);
		if(n>=4 && strncmp(va, "sys:", 4)==0)
			error(0, Ebadarg);
		memcpy(buf, va, n);
		buf[n] = 0;
		if(!postnote(p, 0, buf, NUser))
			error(0, Enonote);
		break;
	default:
		pprint("unknown qid in procwrite\n");
		error(0, Egreg);
	}
	unlock(&p->debug);
	return n;
}

void
procuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void
procerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}
