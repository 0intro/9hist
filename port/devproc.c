#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"fcall.h"
#include	"ureg.h"

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
	"ctl",		{Qctl},		0,			0000,
	"mem",		{Qmem},		0,			0000,
	"note",		{Qnote},	0,			0000,
	"notepg",	{Qnotepg},	0,			0000,
	"proc",		{Qproc},	sizeof(Proc),		0000,
	"segment",	{Qsegment},	0,			0444,
	"status",	{Qstatus},	STATSIZE,		0444,
	"text",		{Qtext},	0,			0000,
};

/* Segment type from portdat.h */
char *sname[]={ "Text", "Data", "Bss", "Stack", "Shared", "Phys" };

/*
 * Qids are, in path:
 *	 4 bits of file type (qids above)
 *	23 bits of process slot number + 1
 *	     in vers,
 *	32 bits of pid, for consistency checking
 * If notepg, c->pgrpid.path is pgrp slot, .vers is noteid.
 */
#define	NPROC	(sizeof procdir/sizeof(Dirtab))
#define	QSHIFT	4	/* location in qid of proc slot # */

#define	QID(q)		(((q).path&0x0000000F)>>0)
#define	SLOT(q)		((((q).path&0x07FFFFFF0)>>QSHIFT)-1)
#define	PID(q)		((q).vers)
#define	NOTEID(q)	((q).vers)

void	procctlreq(Proc*, char*, int);
int	procctlmemio(Proc*, ulong, int, void*, int);
Chan   *proctext(Chan*, Proc*);
Segment *txt2data(Proc*, Segment*);
int	procstopped(void*);

int
procgen(Chan *c, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Proc *p;
	char buf[NAMELEN];
	ulong pid, path, perm;

	if(c->qid.path == CHDIR){
		if(s >= conf.nproc)
			return -1;
		p = proctab(s);
		pid = p->pid;
		if(pid == 0)
			return 0;
		sprint(buf, "%d", pid);
		devdir(c, (Qid){CHDIR|((s+1)<<QSHIFT), pid}, buf, 0, p->user, CHDIR|0555, dp);
		return 1;
	}
	if(s >= NPROC)
		return -1;
	if(tab)
		panic("procgen");
	tab = &procdir[s];
	path = c->qid.path&~(CHDIR|((1<<QSHIFT)-1));	/* slot component */

	p = proctab(SLOT(c->qid));
	perm = tab->perm;
	if(perm == 0)
		perm = p->procmode;

	devdir(c, (Qid){path|tab->qid.path, c->qid.vers}, tab->name, tab->length, p->user, perm, dp);
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

	if(c->qid.path & CHDIR)
		return devopen(c, omode, 0, 0, procgen);

	p = proctab(SLOT(c->qid));
	pg = p->pgrp;
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	omode = openmode(omode);

	switch(QID(c->qid)){
	case Qtext:
		if(omode != OREAD)
			error(Eperm);
		tc = proctext(c, p);
		tc->offset = 0;

		return tc;

	case Qctl:
	case Qnote:
	case Qmem:
	case Qsegment:
	case Qproc:
	case Qstatus:
		break;

	case Qnotepg:
		if(omode!=OWRITE || pg->pgrpid==1)	/* easy to do by mistake */
			error(Eperm);
		c->pgrpid.path = pg->index+1;
		c->pgrpid.vers = p->noteid;
		break;
	default:
		pprint("procopen %lux\n", c->qid);
		error(Egreg);
	}

	/* Affix pid to qid */
	if(p->state != Dead)
		c->qid.vers = p->pid;

	return devopen(c, omode, 0, 0, procgen);
;
}

void
proccreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
procremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
procwstat(Chan *c, char *db)
{
	Proc *p;
	Dir d;

	if(c->qid.path&CHDIR)
		error(Eperm);

	convM2D(db, &d);
	p = proctab(SLOT(c->qid));
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	if(strcmp(u->p->user, p->user) != 0 && strcmp(u->p->user, eve) != 0)
		error(Eperm);

	p->procmode = d.mode&0777;
}

void
procclose(Chan * c)
{
	USED(c);
}

long
procread(Chan *c, void *va, long n, ulong offset)
{
	char *a = va, *b;
	char statbuf[NSEG*32];
	Proc *p;
	Page *pg;
	KMap *k;
	int i, j;
	long l;
	long pid;
	User *up;
	Segment *sg;

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, procgen);

	p = proctab(SLOT(c->qid));
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	switch(QID(c->qid)){
	case Qmem:
		/* ugly math: USERADDR+BY2PG may be == 0 */
		if(offset >= USERADDR && offset <= USERADDR+BY2PG-1) {
			if(offset+n >= USERADDR+BY2PG-1)
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

		if(offset >= KZERO) {
			/* prevent users reading authentication crypt keys */
			if(offset >= pgrpalloc.cryptbase)
			if(offset < pgrpalloc.crypttop)
				error(Eperm);
			/* validate physical kernel addresses */
			if(offset < KZERO+conf.npage0*BY2PG){
				if(offset+n > KZERO+conf.npage0*BY2PG)
					n = KZERO+conf.npage0*BY2PG - offset;
				memmove(a, (char*)offset, n);
				return n;
			}
			if(offset < KZERO+conf.base1+conf.npage1*BY2PG){
				if(offset+n > KZERO+conf.base1+conf.npage1*BY2PG)
					n = KZERO+conf.base1+conf.npage1*BY2PG - offset;
				memmove(a, (char*)offset, n);
				return n;
			}
		}

		return procctlmemio(p, offset, n, va, 1);

	case Qnote:
		qlock(&p->debug);
		if(waserror()){
			qunlock(&p->debug);
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
		if(up->nnote == 0)
			p->notepending = 0;
		kunmap(k);
		poperror();
		qunlock(&p->debug);
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

/* Assertion for deref of psstate which causes faults */
if((p->state < Dead) || (p->state > Rendezvous))
	panic("p->state=#%lux, p->psstate=#%lux\n", p->state, p->psstate);


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
		if(n == 0 && offset == 0)
			exhausted("segments");
		memmove(a, &statbuf[offset], n);
		return n;
	}
	error(Egreg);
}


long
procwrite(Chan *c, void *va, long n, ulong offset)
{
	Proc *p;
	User *up;
	KMap *k;
	char buf[ERRLEN];
	Ureg *ur;
	User *pxu;
	Page *pg;
	char *a = va, *b;
	ulong hi;

	if(c->qid.path & CHDIR)
		error(Eisdir);

	p = proctab(SLOT(c->qid));

	/* Use the remembered noteid in the channel rather than the process pgrpid */
	if(QID(c->qid) == Qnotepg) {
		pgrpnote(NOTEID(c->pgrpid), va, n, NUser);
		return n;
	}

	qlock(&p->debug);
	if(waserror()){
		qunlock(&p->debug);
		nexterror();
	}
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	switch(QID(c->qid)){
	case Qmem:
		if(p->state != Stopped)
			error(Ebadctl);

		if(offset >= USERADDR && offset <= USERADDR+BY2PG-1) {
			pg = p->upage;
			if(pg==0 || p->pid!=PID(c->qid))
				error(Eprocdied);
			k = kmap(pg);
			b = (char*)VA(k);
			pxu = (User*)b;
			hi = offset+n;
			/* Check for floating point registers */
			if(offset >= (ulong)&u->fpsave && hi <= (ulong)&u->fpsave+sizeof(FPsave)){
				memmove(b+(offset-USERADDR), a, n);
				break;
			}
			/* Check user register set for process at kernel entry */
			ur = pxu->dbgreg;
			if(offset < (ulong)ur || hi > (ulong)ur+sizeof(Ureg)) {
				kunmap(k);
				error(Ebadarg);
			}
			ur = (Ureg*)(b+((ulong)ur-USERADDR));
			setregisters(ur, b+(offset-USERADDR), a, n);
			kunmap(k);
		}
		else	/* Try user memory segments */
			n = procctlmemio(p, offset, n, va, 0);
		break;

	case Qctl:
		procctlreq(p, va, n);
		break;

	case Qnote:
		if(p->kp)
			error(Eperm);
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
		memmove(buf, va, n);
		buf[n] = 0;
		if(!postnote(p, 0, buf, NUser))
			exhausted("notes");
		break;

	default:
		pprint("unknown qid in procwrite\n");
		error(Egreg);
	}
	poperror();
	qunlock(&p->debug);
	return n;
}

Chan *
proctext(Chan *c, Proc *p)
{
	Chan *tc;
	Image *i;
	Segment *s;

	s = p->seg[TSEG];
	if(s == 0)
		error(Enonexist);
	if(p->state==Dead)
		error(Eprocdied);

	lock(s);
	i = s->image;
	if(i == 0) {
		unlock(s);
		error(Eprocdied);
	}
	unlock(s);

	lock(i);
	if(waserror()) {
		unlock(i);
		nexterror();
	}

	tc = i->c;
	if(tc == 0)
		error(Eprocdied);

	if(incref(tc) == 1 || (tc->flag&COPEN) == 0 || tc->mode!=OREAD) {
		close(tc);
		error(Eprocdied);
	}

	if(p->pid != PID(c->qid))
		error(Eprocdied);

	unlock(i);
	poperror();

	return tc;
}

void
procstopwait(Proc *p, int ctl)
{
	int pid;

	if(p->pdbg)
		error(Einuse);
	if(procstopped(p))
		return;

	if(ctl != 0)
		p->procctl = ctl;
	p->pdbg = u->p;
	pid = p->pid;
	qunlock(&p->debug);
	u->p->psstate = "Stopwait";
	if(waserror()) {
		p->pdbg = 0;
		qlock(&p->debug);
		nexterror();
	}
	sleep(&u->p->sleep, procstopped, p);
	poperror();
	qlock(&p->debug);
	if(p->pid != pid)
		error(Eprocdied);
}

void
procctlreq(Proc *p, char *va, int n)
{
	char buf[NAMELEN];

	if(n > NAMELEN)
		n = NAMELEN;
	strncpy(buf, va, n);

	if(strncmp(buf, "stop", 4) == 0)
		procstopwait(p, Proc_stopme);
	else if(strncmp(buf, "kill", 4) == 0) {
		if(p->state == Broken)
			ready(p);
		else{
			if(p->state == Stopped)
				ready(p);
			postnote(p, 0, "sys: killed", NExit);
			p->procctl = Proc_exitme;
		}
	}
	else if(strncmp(buf, "hang", 4) == 0)
		p->hang = 1;
	else if(strncmp(buf, "waitstop", 8) == 0)
		procstopwait(p, 0);
	else if(strncmp(buf, "startstop", 9) == 0) {
		if(p->state != Stopped)
			error(Ebadctl);
		p->procctl = Proc_traceme;
		ready(p);
		procstopwait(p, Proc_traceme);
	}
	else if(strncmp(buf, "start", 5) == 0) {
		if(p->state != Stopped)
			error(Ebadctl);
		ready(p);
	}
	else
		error(Ebadctl);
}

int
procstopped(void *a)
{
	Proc *p = a;
	return p->state == Stopped;
}

int
procctlmemio(Proc *p, ulong offset, int n, void *va, int read)
{
	Pte *pte;
	Page *pg;
	KMap *k;
	Segment *s;
	ulong soff, l;
	char *a = va, *b;

	for(;;) {
		s = seg(p, offset, 1);
		if(s == 0)
			error(Ebadarg);

		if(offset+n >= s->top)
			n = s->top-offset;

		if((s->type&SG_TYPE) == SG_TEXT)
			s = txt2data(p, s);

		s->steal++;
		soff = offset-s->base;
		if(waserror()) {
			s->steal--;
			nexterror();
		}
		if(fixfault(s, offset, read, 0) == 0)
			break;
		poperror();
		s->steal--;
	}
	poperror();
	pte = s->map[soff/PTEMAPMEM];
	if(pte == 0)
		panic("procctlmemio"); 
	pg = pte->pages[(soff&(PTEMAPMEM-1))/BY2PG];
	if(pagedout(pg))
		panic("procctlmemio1"); 

	l = BY2PG - (offset&(BY2PG-1));
	if(n > l)
		n = l;

	k = kmap(pg);
	b = (char*)VA(k);
	if(read == 1)
		memmove(a, b+(offset&(BY2PG-1)), n);
	else
		memmove(b+(offset&(BY2PG-1)), a, n);

	kunmap(k);

	s->steal--;
	qunlock(&s->lk);

	if(read == 0)
		p->newtlb = 1;

	return n;
}

Segment *
txt2data(Proc *p, Segment *s)
{
	Segment *ps;
	int i;

	ps = newseg(SG_DATA, s->base, s->size);
	ps->image = s->image;
	incref(ps->image);
	ps->fstart = s->fstart;
	ps->flen = s->flen;
	ps->flushme = 1;

	for(i = 0; i < NSEG; i++)
		if(p->seg[i] == s)
			break;
	if(p->seg[i] != s)
		panic("segment gone");

	qunlock(&s->lk);
	putseg(s);
	qlock(&ps->lk);
	p->seg[i] = ps;

	return ps;
}
