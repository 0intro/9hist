#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"ureg.h"

enum
{
	Qctl,
	Qdir,
	Qfpregs,
	Qkregs,
	Qmem,
	Qnote,
	Qnoteid,
	Qnotepg,
	Qns,
	Qproc,
	Qregs,
	Qsegment,
	Qstatus,
	Qtext,
	Qwait,
	Qprofile,
};

#define	STATSIZE	(2*NAMELEN+12+9*12)
Dirtab procdir[] =
{
	"ctl",		{Qctl},		0,			0000,
	"fpregs",	{Qfpregs},	sizeof(FPsave),		0000,
	"kregs",	{Qkregs},	sizeof(Ureg),		0000,
	"mem",		{Qmem},		0,			0000,
	"note",		{Qnote},	0,			0000,
	"noteid",	{Qnoteid},	0,			0666,
	"notepg",	{Qnotepg},	0,			0000,
	"ns",		{Qns},		0,			0400,
	"proc",		{Qproc},	0,			0400,
	"regs",		{Qregs},	sizeof(Ureg),		0000,
	"segment",	{Qsegment},	0,			0444,
	"status",	{Qstatus},	STATSIZE,		0444,
	"text",		{Qtext},	0,			0000,
	"wait",		{Qwait},	0,			0400,
	"profile",	{Qprofile},	0,			0400,
};

/* Segment type from portdat.h */
char *sname[]={ "Text", "Data", "Bss", "Stack", "Shared", "Phys", "Shdata" };

/*
 * Qids are, in path:
 *	 4 bits of file type (qids above)
 *	23 bits of process slot number + 1
 *	     in vers,
 *	32 bits of pid, for consistency checking
 * If notepg, c->pgrpid.path is pgrp slot, .vers is noteid.
 */
#define	QSHIFT	4	/* location in qid of proc slot # */

#define	QID(q)		(((q).path&0x0000000F)>>0)
#define	SLOT(q)		((((q).path&0x07FFFFFF0)>>QSHIFT)-1)
#define	PID(q)		((q).vers)
#define	NOTEID(q)	((q).vers)

void	procctlreq(Proc*, char*, int);
int	procctlmemio(Proc*, ulong, int, void*, int);
Chan*	proctext(Chan*, Proc*);
Segment* txt2data(Proc*, Segment*);
int	procstopped(void*);
void	mntscan(Mntwalk*);

static int
procgen(Chan *c, Dirtab *tab, int, int s, Dir *dp)
{
	Qid qid;
	Proc *p;
	Segment *q;
	char buf[NAMELEN];
	ulong pid, path, perm, len;

	if(c->qid.path == CHDIR){
		if(s >= conf.nproc)
			return -1;
		p = proctab(s);
		pid = p->pid;
		if(pid == 0)
			return 0;
		sprint(buf, "%d", pid);
		qid = (Qid){CHDIR|((s+1)<<QSHIFT), pid};
		devdir(c, qid, buf, 0, p->user, CHDIR|0555, dp);
		return 1;
	}
	if(s >= nelem(procdir))
		return -1;
	if(tab)
		panic("procgen");

	tab = &procdir[s];
	path = c->qid.path&~(CHDIR|((1<<QSHIFT)-1));	/* slot component */

	p = proctab(SLOT(c->qid));
	perm = tab->perm;
	if(perm == 0)
		perm = p->procmode;

	len = tab->length;
	switch(QID(c->qid)) {
	case Qwait:
		len = p->nwait * sizeof(Waitmsg);
		break;
	case Qprofile:
		q = p->seg[TSEG];
		if(q && q->profile) {
			len = (q->top-q->base)>>LRESPROF;
			len *= sizeof(*q->profile);
		}
		break;
	}

	qid = (Qid){path|tab->qid.path, c->qid.vers};
	devdir(c, qid, tab->name, len, p->user, perm, dp);
	return 1;
}

static void
procinit(void)
{
	if(conf.nproc >= (1<<(16-QSHIFT))-1)
		print("warning: too many procs for devproc\n");
}

static Chan*
procattach(char *spec)
{
	return devattach('p', spec);
}

static int
procwalk(Chan *c, char *name)
{
	if(strcmp(name, "..") == 0) {
		c->qid.path = CHDIR;
		return 1;
	}

	return devwalk(c, name, 0, 0, procgen);
}

static void
procstat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, procgen);
}

static Chan*
procopen(Chan *c, int omode)
{
	Proc *p;
	Pgrp *pg;
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

	case Qproc:
	case Qkregs:
	case Qsegment:
	case Qprofile:
		if(omode != OREAD)
			error(Eperm);
		break;

	case Qctl:
	case Qnote:
	case Qnoteid:
	case Qmem:
	case Qstatus:
	case Qwait:
	case Qregs:
	case Qfpregs:
		break;

	case Qns:
		if(omode != OREAD)
			error(Eperm);
		c->aux = malloc(sizeof(Mntwalk));
		break;

	case Qnotepg:
		if(omode!=OWRITE || pg->pgrpid == 1)
			error(Eperm);
		c->pgrpid.path = pg->pgrpid+1;
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
}

static void
procwstat(Chan *c, char *db)
{
	Proc *p;
	Dir d;

	if(c->qid.path&CHDIR)
		error(Eperm);

	p = proctab(SLOT(c->qid));
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	if(strcmp(up->user, p->user) != 0 && strcmp(up->user, eve) != 0)
		error(Eperm);

	convM2D(db, &d);
	p->procmode = d.mode&0777;
}

static void
procclose(Chan * c)
{
	if(QID(c->qid) == Qns && c->aux != 0)
		free(c->aux);
}

static long
procread(Chan *c, void *va, long n, ulong offset)
{
	long l;
	Proc *p;
	Waitq *wq;
	Ureg kur;
	uchar *rptr;
	Mntwalk *mw;
	Segment *sg, *s;
	char *a = va, *sps;
	int i, j, rsize, pid;
	char statbuf[NSEG*32];

	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, 0, 0, procgen);

	p = proctab(SLOT(c->qid));
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	switch(QID(c->qid)){
	case Qmem:
		if(offset < KZERO
		|| (offset >= USTKTOP-USTKSIZE && offset < USTKTOP))
			return procctlmemio(p, offset, n, va, 1);

		/* Protect crypt key memory */
		if(offset >= palloc.cmembase&&offset < palloc.cmemtop)
			error(Eperm);

		/* validate physical kernel addresses */
		if(offset < (ulong)end) {
			if(offset+n > (ulong)end)
				n = (ulong)end - offset;
			memmove(a, (char*)offset, n);
			return n;
		}
		if(offset >= conf.base0 && offset < conf.npage0){
			if(offset+n > conf.npage0)
				n = conf.npage0 - offset;
			memmove(a, (char*)offset, n);
			return n;
		}
		if(offset >= conf.base1 && offset < conf.npage1){
			if(offset+n > conf.npage1)
				n = conf.npage1 - offset;
			memmove(a, (char*)offset, n);
			return n;
		}
		error(Ebadarg);
	case Qprofile:
		s = p->seg[TSEG];
		if(s == 0 || s->profile == 0)
			error("profile is off");
		i = (s->top-s->base)>>LRESPROF;
		i *= sizeof(*s->profile);
		if(offset >= i)
			return 0;
		if(offset+n > i)
			n = i - offset;
		memmove(a, ((char*)s->profile)+offset, n);
		return n;

	case Qnote:
		qlock(&p->debug);
		if(waserror()){
			qunlock(&p->debug);
			nexterror();
		}
		if(p->pid != PID(c->qid))
			error(Eprocdied);
		if(n < ERRLEN)
			error(Etoosmall);
		if(p->nnote == 0)
			n = 0;
		else {
			memmove(va, p->note[0].msg, ERRLEN);
			p->nnote--;
			memmove(p->note, p->note+1, p->nnote*sizeof(Note));
			n = ERRLEN;
		}
		if(p->nnote == 0)
			p->notepending = 0;
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

	case Qregs:
		rptr = (uchar*)p->dbgreg;
		rsize = sizeof(Ureg);
		goto regread;

	case Qkregs:
		memset(&kur, 0, sizeof(Ureg));
		setkernur(&kur, p);
		rptr = (uchar*)&kur;
		rsize = sizeof(Ureg);
		goto regread;

	case Qfpregs:
		rptr = (uchar*)&p->fpsave;
		rsize = sizeof(FPsave);
	regread:
		if(offset >= rsize)
			return 0;
		if(offset+n > rsize)
			n = rsize - offset;
		memmove(a, rptr+offset, n);
		return n;

	case Qstatus:
		if(offset >= STATSIZE)
			return 0;
		if(offset+n > STATSIZE)
			n = STATSIZE - offset;

		sps = p->psstate;
		if(sps == 0)
			sps = statename[p->state];
		memset(statbuf, ' ', sizeof statbuf);
		memmove(statbuf+0*NAMELEN, p->text, strlen(p->text));
		memmove(statbuf+1*NAMELEN, p->user, strlen(p->user));
		memmove(statbuf+2*NAMELEN, sps, strlen(sps));
		j = 2*NAMELEN + 12;

		for(i = 0; i < 6; i++) {
			l = p->time[i];
			if(i == TReal)
				l = MACHP(0)->ticks - l;
			l = TK2MS(l);
			readnum(0, statbuf+j+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		/* ignore stack, which is mostly non-existent */
		l = 0;
		for(i=1; i<NSEG; i++){
			s = p->seg[i];
			if(s)
				l += s->top - s->base;
		}
		readnum(0, statbuf+j+NUMSIZE*6, NUMSIZE, l>>10, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*7, NUMSIZE, p->basepri, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*8, NUMSIZE, p->priority, NUMSIZE);
		memmove(a, statbuf+offset, n);
		return n;

	case Qsegment:
		j = 0;
		for(i = 0; i < NSEG; i++) {
			sg = p->seg[i];
			if(sg == 0)
				continue;
			j += sprint(statbuf+j, "%-6s %c%c %.8lux %.8lux %4d\n",
				sname[sg->type&SG_TYPE],
				sg->type&SG_RONLY ? 'R' : ' ',
				sg->profile ? 'P' : ' ',
				sg->base, sg->top, sg->ref);
		}
		if(offset >= j)
			return 0;
		if(offset+n > j)
			n = j-offset;
		if(n == 0 && offset == 0)
			exhausted("segments");
		memmove(a, &statbuf[offset], n);
		return n;

	case Qwait:
		if(n < sizeof(Waitmsg))
			error(Etoosmall);

		if(!canqlock(&p->qwaitr))
			error(Einuse);

		if(waserror()) {
			qunlock(&p->qwaitr);
			nexterror();
		}

		lock(&p->exl);
		if(up == p && p->nchild == 0 && p->waitq == 0) {
			unlock(&p->exl);
			error(Enochild);
		}
		pid = p->pid;
		while(p->waitq == 0) {
			unlock(&p->exl);
			sleep(&p->waitr, haswaitq, p);
			if(p->pid != pid)
				error(Eprocdied);
			lock(&p->exl);
		}
		wq = p->waitq;
		p->waitq = wq->next;
		p->nwait--;
		unlock(&p->exl);

		qunlock(&p->qwaitr);
		poperror();
		memmove(a, &wq->w, sizeof(Waitmsg));
		free(wq);
		return sizeof(Waitmsg);

	case Qns:
		mw = c->aux;
		mntscan(mw);
		if(mw->mh == 0)
			return 0;
		if(n < NAMELEN+11)
			error(Etoosmall);
		i = sprint(a, "%s %d ", mw->cm->spec, mw->cm->flag);
		n -= i;
		a += i;
		i = ptpath(mw->mh->from->path, a, n);
		n -= i;
		a += i;
		if(n > 0) {
			*a++ = ' ';
			n--;
		}
		a += ptpath(mw->cm->to->path, a, n);
		return a - (char*)va;
	case Qnoteid:
		return readnum(offset, va, n, p->noteid, NUMSIZE);
	}
	error(Egreg);
	return 0;		/* not reached */
}

void
mntscan(Mntwalk *mw)
{
	Pgrp *pg;
	Mount *t;
	Mhead *f;
	int nxt, i;
	ulong last, bestmid;

	pg = up->pgrp;
	rlock(&pg->ns);

	nxt = 0;
	bestmid = ~0;

	last = 0;
	if(mw->mh)
		last = mw->cm->mountid;

	for(i = 0; i < MNTHASH; i++) {
		for(f = pg->mnthash[i]; f; f = f->hash) {
			for(t = f->mount; t; t = t->next) {
				if(mw->mh == 0 ||
				  (t->mountid > last && t->mountid < bestmid)) {
					mw->cm = t;
					mw->mh = f;
					bestmid = mw->cm->mountid;
					nxt = 1;
				}
			}
		}
	}
	if(nxt == 0)
		mw->mh = 0;

	runlock(&pg->ns);
}

static long
procwrite(Chan *c, void *va, long n, ulong offset)
{
	int id;
	Proc *p, *t, *et;
	char *a, buf[ERRLEN];

	a = va;
	if(c->qid.path & CHDIR)
		error(Eisdir);

	p = proctab(SLOT(c->qid));

	/* Use the remembered noteid in the channel rather
	 * than the process pgrpid
	 */
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

		n = procctlmemio(p, offset, n, va, 0);
		break;

	case Qregs:
		if(offset >= sizeof(Ureg))
			return 0;
		if(offset+n > sizeof(Ureg))
			n = sizeof(Ureg) - offset;
		setregisters(p->dbgreg, (char*)(p->dbgreg)+offset, va, n);
		break;

	case Qfpregs:
		if(offset >= sizeof(FPsave))
			return 0;
		if(offset+n > sizeof(FPsave))
			n = sizeof(FPsave) - offset;
		memmove((uchar*)&p->fpsave+offset, va, n);
		break;

	case Qctl:
		procctlreq(p, va, n);
		break;

	case Qnote:
		if(p->kp)
			error(Eperm);
		if(n >= ERRLEN-1)
			error(Etoobig);
		memmove(buf, va, n);
		buf[n] = 0;
		if(!postnote(p, 0, buf, NUser))
			error("note not posted");
		break;
	case Qnoteid:
		id = atoi(a);
		if(id == p->pid) {
			p->noteid = id;
			break;
		}
		t = proctab(0);
		for(et = t+conf.nproc; t < et; t++) {
			if(id == t->noteid) {
				if(strcmp(p->user, t->user) != 0)
					error(Eperm);
				p->noteid = id;
				break;
			}
		}
		if(p->noteid != id)
			error(Ebadarg);
		break;
	default:
		pprint("unknown qid in procwrite\n");
		error(Egreg);
	}
	poperror();
	qunlock(&p->debug);
	return n;
}

Dev procdevtab = {
	'p',
	"proc",

	devreset,
	procinit,
	procattach,
	devclone,
	procwalk,
	procstat,
	procopen,
	devcreate,
	procclose,
	procread,
	devbread,
	procwrite,
	devbwrite,
	devremove,
	procwstat,
};

Chan*
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
		cclose(tc);
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
	p->pdbg = up;
	pid = p->pid;
	qunlock(&p->debug);
	up->psstate = "Stopwait";
	if(waserror()) {
		p->pdbg = 0;
		qlock(&p->debug);
		nexterror();
	}
	sleep(&up->sleep, procstopped, p);
	poperror();
	qlock(&p->debug);
	if(p->pid != pid)
		error(Eprocdied);
}

void
procctlfgrp(Fgrp *f)
{
	int i;
	Chan *c;

	lock(f);
	f->ref++;
	for(i = 0; i < f->maxfd; i++) {
		c = f->fd[i];
		if(c != 0) {
			f->fd[i] = 0;
			unlock(f);
			cclose(c);
			lock(f);
		}
	}
	unlock(f);
	closefgrp(f);
}


void
procctlreq(Proc *p, char *va, int n)
{
	Segment *s;
	int i, npc;
	char buf[NAMELEN];

	if(n > NAMELEN)
		n = NAMELEN;
	strncpy(buf, va, n);

	if(strncmp(buf, "stop", 4) == 0)
		procstopwait(p, Proc_stopme);
	else
	if(strncmp(buf, "kill", 4) == 0) {
		switch(p->state) {
		case Broken:
			unbreak(p);
			break;
		case Stopped:
			postnote(p, 0, "sys: killed", NExit);
			p->procctl = Proc_exitme;
			ready(p);
			break;
		default:
			postnote(p, 0, "sys: killed", NExit);
			p->procctl = Proc_exitme;
		}
	}
	else
	if(strncmp(buf, "hang", 4) == 0)
		p->hang = 1;
	else
	if(strncmp(buf, "nohang", 6) == 0)
		p->hang = 0;
	else
	if(strncmp(buf, "waitstop", 8) == 0)
		procstopwait(p, 0);
	else
	if(strncmp(buf, "startstop", 9) == 0) {
		if(p->state != Stopped)
			error(Ebadctl);
		p->procctl = Proc_traceme;
		ready(p);
		procstopwait(p, Proc_traceme);
	}
	else
	if(strncmp(buf, "start", 5) == 0) {
		if(p->state != Stopped)
			error(Ebadctl);
		ready(p);
	}
	else
	if(strncmp(buf, "closefgrp", 9) == 0)
		procctlfgrp(p->fgrp);
	else
	if(strncmp(buf, "pri", 3) == 0) {
		if(n < 4)
			error(Ebadctl);
		i = atoi(buf+4);
		if(i < 0)
			i = 0;
		if(i >= Nrq)
			i = Nrq - 1;
		if(i < p->basepri && !iseve())
			error(Eperm);
		p->basepri = i;
	}
	else
	if(strncmp(buf, "wired", 5) == 0) {
		if(n < 6)
			error(Ebadctl);
		i = atoi(buf+6);
		if(i)
			procwired(p);
	}
	else
	if(strncmp(buf, "profile", 7) == 0) {
		s = p->seg[TSEG];
		if(s == 0 || (s->type&SG_TYPE) != SG_TEXT)
			error(Ebadctl);
		if(s->profile != 0)
			free(s->profile);
		npc = (s->top-s->base)>>LRESPROF;
		s->profile = malloc(npc*sizeof(*s->profile));
		if(s->profile == 0)
			error(Enomem);
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
	KMap *k;
	Pte *pte;
	Page *pg;
	Segment *s;
	ulong soff, l;
	char *a = va, *b;

	for(;;) {
		s = seg(p, offset, 1);
		if(s == 0)
			error(Ebadarg);

		if(offset+n >= s->top)
			n = s->top-offset;

		if(!read && (s->type&SG_TYPE) == SG_TEXT)
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
	b += offset&(BY2PG-1);
	if(read == 1)
		memmove(a, b, n);
	else
		memmove(b, a, n);
	kunmap(k);

	/* Ensure the process sees text page changes */
	if(s->flushme)
		memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));

	s->steal--;

	if(read == 0)
		p->newtlb = 1;

	return n;
}

Segment*
txt2data(Proc *p, Segment *s)
{
	int i;
	Segment *ps;

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

Segment*
data2txt(Segment *s)
{
	Segment *ps;

	ps = newseg(SG_TEXT, s->base, s->size);
	ps->image = s->image;
	incref(ps->image);
	ps->fstart = s->fstart;
	ps->flen = s->flen;
	ps->flushme = 1;

	return ps;
}
