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
	Qsegment,
	Qstatus,
	Qtext,
};

#define	STATSIZE	(2*NAMELEN+12+6*12)
Dirtab procdir[]={
	"ctl",		{Qctl},		0,			0600,
	"mem",		{Qmem},		0,			0600,
	"note",		{Qnote},	0,			0600,
	"notepg",	{Qnotepg},	0,			0200,
	"proc",		{Qproc},	sizeof(Proc),		0600,
	"segment",	{Qsegment},	0,			0400,
	"status",	{Qstatus},	STATSIZE,		0400,
	"text",		{Qtext},	0,			0600,
};

char *sname[]={ 			/* Segment type from portdat.h */
	"Text", 
	"Data", 
	"Bss", 
	"Stack", 
	"Shared", 
	"Phys",
};

/*
 * Qids are, in path:
 *	 4 bits of file type (qids above)
 *	23 bits of process slot number + 1
 *	     in vers,
 *	32 bits of pid, for consistency checking
 * If notepg, c->pgrpid.path is pgrp slot, .vers is pgrpid.
 */
#define	NPROC	(sizeof procdir/sizeof(Dirtab))
#define	QSHIFT	4	/* location in qid of proc slot # */
#define	QID(q)	(((q).path&0x0000000F)>>0)
#define	SLOT(q)	((((q).path&0x07FFFFFF0)>>QSHIFT)-1)
#define	PID(q)	((q).vers)

int
procgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Proc *p;
	char buf[NAMELEN];
	ulong pid, path;

	if(c->qid.path == CHDIR){
		if(s >= conf.nproc)
			return -1;
		p = proctab(s);
		pid = p->pid;
		if(pid == 0)
			return 0;
		sprint(buf, "%d", pid);
		devdir(c, (Qid){CHDIR|((s+1)<<QSHIFT), pid}, buf, 0, CHDIR|0500, dp);
		return 1;
	}
	if(s >= NPROC)
		return -1;
	if(tab)
		panic("procgen");
	tab = &procdir[s];
	path = c->qid.path&~(CHDIR|((1<<QSHIFT)-1));	/* slot component */
	devdir(c, (Qid){path|tab->qid.path, c->qid.vers},
		tab->name, tab->length, tab->perm, dp);
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
	if(strcmp(name, "..") == 0) {
		c->qid.path = Qdir|CHDIR;
		return 1;
	}

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
	Segment *s;
	Chan *tc;

	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
		goto done;
	}
	p = proctab(SLOT(c->qid));
	pg = p->pgrp;
	if(p->pid != PID(c->qid))
    Died:
		error(Eprocdied);
	omode = openmode(omode);

	switch(QID(c->qid)){
	case Qtext:
		s = p->seg[TSEG];
		if(s==0 || p->state==Dead || s->image==0) 
			goto Died;

		tc = s->image->c;
		if(tc == 0)
			goto Died;

		if(incref(tc) == 0){
    Close:
			close(tc);
			goto Died;
		}
		if(!(tc->flag&COPEN) || tc->mode!=OREAD)
			goto Close;

		if(p->pid != PID(c->qid))
			goto Close;

		tc->offset = 0;
		return tc;
	case Qctl:
	case Qnote:
	case Qsegment:
		break;

	case Qnotepg:
		if(omode!=OWRITE || pg->pgrpid==1)	/* easy to do by mistake */
			error(Eperm);
		c->pgrpid.path = pg->index+1;
		c->pgrpid.vers = pg->pgrpid;
		break;

	case Qdir:
	case Qmem:
	case Qproc:
	case Qstatus:
		if(omode != OREAD)
			error(Eperm);
		break;
	default:
		pprint("unknown qid in devopen\n");
		error(Egreg);
	}
	/*
	 * Affix pid to qid
	 */
	if(p->state != Dead)
		c->qid.vers = p->pid;
   done:
	c->mode = omode;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
proccreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
procremove(Chan *c)
{
	error(Eperm);
}

void
procwstat(Chan *c, char *db)
{
	error(Eperm);
}

void
procclose(Chan * c)
{
}

long
procread(Chan *c, void *va, long n, ulong offset)
{
	char *a = va, *b;
	char statbuf[NSEG*32];
	Proc *p;
	Pte *pte;
	Segment *s;
	Page *pg;
	KMap *k;
	int i, j;
	long l;
	long pid;
	User *up;
	ulong soff;
	Segment *sg;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, procgen);

	/*
	 * BUG: should lock(&p->debug)?
	 */
	p = proctab(SLOT(c->qid));
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	switch(QID(c->qid)){
	case Qmem:
		/*
		 * One page at a time
		 */
		if(((offset+n)&~(BY2PG-1)) != (offset&~(BY2PG-1)))
			n = BY2PG - (offset&(BY2PG-1));
		s = seg(p, offset, 1);
		if(s){
			if(p->pid!=PID(c->qid)){
				qunlock(&s->lk);
				error(Eprocdied);
			}

			soff = offset-s->base;
			pte = s->map[soff/PTEMAPMEM];
			pg = 0;
			if(pte)
				pg = pte->pages[(soff&(PTEMAPMEM-1))/BY2PG];

			s->steal++;
			qunlock(&s->lk);
			if(pagedout(pg)){
				pprint("nonresident page addr %lux (complain to philw)\n", offset);
				memset(a, 0, n);
			}else{
				k = kmap(pg);
				b = (char*)VA(k);
				memmove(a, b+(offset&(BY2PG-1)), n);
				kunmap(k);
			}
			qlock(&s->lk);
			s->steal--;
			qunlock(&s->lk);
			return n;
		}
		/* u area */
		if(offset>=USERADDR && offset<USERADDR+BY2PG){
			if(offset+n > USERADDR+BY2PG)
				n = USERADDR+BY2PG - offset;
			pg = p->upage;
			if(pg==0 || p->pid!=PID(c->qid))
				error(Eprocdied);
			k = kmap(pg);
			b = (char*)VA(k);
			memmove(a, b+(offset-USERADDR), n);
			kunmap(k);
			return n;
		}

		/* kernel memory.  BUG: shouldn't be so easygoing. BUG: mem mapping? */
		if(offset>=KZERO && offset<KZERO+conf.npage0*BY2PG){
			if(offset+n > KZERO+conf.npage0*BY2PG)
				n = KZERO+conf.npage0*BY2PG - offset;
			memmove(a, (char*)offset, n);
			return n;
		}
		if(offset>=KZERO && offset<KZERO+conf.base1+conf.npage1*BY2PG){
			if(offset+n > KZERO+conf.base1+conf.npage1*BY2PG)
				n = KZERO+conf.base1+conf.npage1*BY2PG - offset;
			memmove(a, (char*)offset, n);
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
		if(p->pid != PID(c->qid))
			error(Eprocdied);
		k = kmap(p->upage);
		up = (User*)VA(k);
		if(up->p != p){
			kunmap(k);
			pprint("note read u/p mismatch");
			error(Egreg);
		}
		if(n < ERRLEN)
			error(Etoosmall);
		if(up->nnote == 0)
			n = 0;
		else{
			memmove(va, up->note[0].msg, ERRLEN);
			up->nnote--;
			memmove(&up->note[0], &up->note[1], up->nnote*sizeof(Note));
			n = ERRLEN;
		}
		kunmap(k);
		poperror();
		unlock(&p->debug);
		return n;

	case Qproc:
		if(offset >= sizeof(Proc))
			return 0;
		if(offset+n > sizeof(Proc))
			n = sizeof(Proc) - offset;
		memmove(a, ((char*)p)+offset, n);
		return n;

	case Qstatus:
		if(offset >= STATSIZE)
			return 0;
		if(offset+n > STATSIZE)
			n = STATSIZE - offset;
		j = sprint(statbuf, "%-27s %-27s %-11s ",
			p->text, p->user, p->psstate ? p->psstate : statename[p->state]);
		for(i=0; i<6; i++){
			l = p->time[i];
			if(i == TReal)
				l = MACHP(0)->ticks - l;
			l = TK2MS(l);
			readnum(0, statbuf+j+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		memmove(a, statbuf+offset, n);
		return n;
	case Qsegment:
		j = 0;
		for(i = 0; i < NSEG; i++)
			if(sg = p->seg[i])
				j += sprint(&statbuf[j], "%-6s %c %.8lux %.8lux %4d\n",
				sname[sg->type&SG_TYPE], sg->type&SG_RONLY ? 'R' : ' ',
				sg->base, sg->top, sg->ref);
		if(offset >= j)
			return 0;
		if(offset+n > j)
			n = j-offset;
		memmove(a, &statbuf[offset], n);
		return n;
	}
	error(Egreg);
}


long
procwrite(Chan *c, void *va, long n, ulong offset)
{
	Proc *p;
	Pgrp *pg;
	User *up;
	KMap *k;
	char buf[ERRLEN];

	if(c->qid.path & CHDIR)
		error(Eisdir);

	p = proctab(SLOT(c->qid));
	/*
	 * Special case: don't worry about process, just use remembered group
	 */
	if(QID(c->qid) == Qnotepg){
		pg = pgrptab(c->pgrpid.path-1);
		qlock(&pg->debug);
		if(waserror()){
			qunlock(&pg->debug);
			nexterror();
		}
		if(pg->pgrpid != c->pgrpid.vers){
			qunlock(&pg->debug);
  	  		goto Died;
		}
		pgrpnote(pg, va, n, NUser);
		poperror();
		qunlock(&pg->debug);
		return n;
	}

	lock(&p->debug);
	if(waserror()){
		unlock(&p->debug);
		nexterror();
	}
	if(p->pid != PID(c->qid))
    Died:
		error(Eprocdied);

	switch(QID(c->qid)){
	case Qctl:
		if(n >= 4 && strncmp(va, "exit", 4) == 0) {
			if(p->state == Broken) {
				ready(p);
				break;
			}
		}
		else
		if(n >= 4 && strncmp(va, "stop", 4) == 0) {
			p->procctl = Proc_stopme;
			break;
		}
		else
		if(n >= 5 && strncmp(va, "start", 5) == 0) {
			if(p->state == Stopped) {
				ready(p);
				break;
			}
			errors("not stopped");
		}
		else
		if(n >= 4 && strncmp(va, "kill", 4) == 0) {
			p->procctl = Proc_exitme;
			break;
		}
		error(Ebadctl);
	case Qnote:
		k = kmap(p->upage);
		up = (User*)VA(k);
		if(up->p != p){
			kunmap(k);
			pprint("note write u/p mismatch");
			error(Egreg);
		}
		kunmap(k);
		if(n >= ERRLEN-1)
			error(Etoobig);
		if(n>=4 && strncmp(va, "sys:", 4)==0)
			error(Ebadarg);
		memmove(buf, va, n);
		buf[n] = 0;
		if(!postnote(p, 0, buf, NUser))
			error(Enonote);
		break;
	default:
		pprint("unknown qid in procwrite\n");
		error(Egreg);
	}
	poperror();
	unlock(&p->debug);
	return n;
}
