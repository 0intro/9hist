#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

Ref	pidalloc;
Ref	noteidalloc;

struct
{
	Lock;
	Proc	*arena;
	Proc	*free;
}procalloc;

struct
{
	Lock;
	Waitq	*free;
}waitqalloc;

typedef struct
{
	Lock;
	Proc	*head;
	Proc	*tail;
	int	n;
} Schedq;

int	nrdy;
int	lastreadied;
Schedq	runq[Nrq];

char *statename[] =
{			/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"Wakeme",
	"Broken",
	"Stopped",
	"Rendez",
};

/*
 * Always splhi()'ed.
 */
void
schedinit(void)		/* never returns */
{
	setlabel(&m->sched);
	if(up) {
		m->proc = 0;
		switch(up->state) {
		case Running:
			ready(up);
			break;
		case Moribund:
			up->state = Dead;
			/* 
			 * Holding locks from pexit:
			 * 	procalloc
			 */
			mmurelease(up);

			up->qnext = procalloc.free;
			procalloc.free = up;
			unlock(&procalloc);
			break;
		}
		up->mach = 0;
		up = 0;
	}
	sched();
}

void
sched(void)
{
	if(up) {
		splhi();

		/* statistics */
		m->cs++;

		procsave(up);
		if(setlabel(&up->sched)) {
			procrestore(up);
			spllo();
			return;
		}
		gotolabel(&m->sched);
	}
	up = runproc();
	up->state = Running;
	up->mach = MACHP(m->machno);
	m->proc = up;
	mmuswitch(up);
	gotolabel(&up->sched);
}

int
anyready(void)
{
	return nrdy;
}

int
anyhigher(void)
{
	int x;

	x = lastreadied;
	lastreadied = 0;
	return nrdy && x >= up->priority;
}

enum
{
	Squantum = (HZ+Nrq-1)/Nrq,
};

void
ready(Proc *p)
{
	int s, pri;
	Schedq *rq;

	s = splhi();

	/* history counts */
	if(p->state == Running){
		p->rt++;
		pri = ((p->art + (p->rt<<1))>>2)/Squantum;
	} else {
		p->art = (p->art + (p->rt<<1))>>2;
		p->rt = 0;
		pri = p->art/Squantum;
	}
	pri = p->basepri - pri;
	if(pri < 0)
		pri = 0;

	/* the only intersection between the classes is at PriNormal */
	if(pri < PriNormal && p->basepri > PriNormal)
		pri = PriNormal;
	p->priority = pri;
	rq = &runq[p->priority];

	lock(runq);
	p->rnext = 0;
	if(rq->tail)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;
	rq->n++;
	nrdy++;
	p->readytime = m->ticks;
	p->state = Ready;
	if(p->priority > lastreadied)
		lastreadied = p->priority;
	unlock(runq);
	splx(s);
}

Proc*
runproc(void)
{
	int i;
	Schedq *rq;
	Proc *p, *l;

loop:

	/*
	 *  find a process that last ran on this processor (affinity),
	 *  or one that hasn't moved in a while (load balancing).
	 */
	spllo();
	for(;;){
		/*
		 *  Once a second we look for a long waiting process
		 *  in the lowest priority queue to make sure nothing
		 *  gets starved out by a malfunctioning high priority
		 *  process.
		 */
		if((m->ticks % HZ) == 0){
			for(rq = runq; rq < &runq[Nrq]; rq++){
				p = rq->head;
				if(p == 0 || p->mp != MACHP(m->machno))
					continue;

				i = m->ticks - p->readytime;
				if(i < HZ)
					break;

				p->art = 0;
				goto found;
			}
		}

		/*
		 *  get highest priority process that this
		 *  processor can run given affinity constraints
		 */
		for(rq = &runq[Nrq-1]; rq >= runq; rq--){
			p = rq->head;
			if(p == 0)
				continue;
			for(; p; p = p->rnext){
				if(p->mp == MACHP(m->machno) || p->movetime < m->ticks)
					goto found;
			}
		}
	}


found:
	splhi();
	lock(runq);

	l = 0;
	for(p = rq->head; p; p = p->rnext){
		if(p->mp == MACHP(m->machno) || p->movetime < m->ticks)
			break;
		l = p;
	}

	/*
	 *  p->mach==0 only when process state is saved
	 */
	if(p == 0 || p->mach){	
		unlock(runq);
		goto loop;
	}
	if(p->rnext == 0)
		rq->tail = l;
	if(l)
		l->rnext = p->rnext;
	else
		rq->head = p->rnext;
	rq->n--;
	nrdy--;
	if(p->state != Ready)
		print("runproc %s %d %s\n", p->text, p->pid, statename[p->state]);
	unlock(runq);

	p->state = Scheding;
	if(p->mp != MACHP(m->machno))
		p->movetime = m->ticks + HZ/2;
	p->mp = MACHP(m->machno);
	return p;
}

int
canpage(Proc *p)
{
	int ok = 0;

	splhi();
	lock(runq);
	/* Only reliable way to see if we are Running */
	if(p->mach == 0) {
		p->newtlb = 1;
		ok = 1;
	}
	unlock(runq);
	spllo();

	return ok;
}

Proc*
newproc(void)
{
	Proc *p;

	lock(&procalloc);
	for(;;) {
		if(p = procalloc.free)
			break;

		unlock(&procalloc);
		resrcwait("no procs");
		lock(&procalloc);
	}
	procalloc.free = p->qnext;
	unlock(&procalloc);

	procalloc.free = p->qnext;
	p->state = Scheding;
	p->psstate = "New";
	p->mach = 0;
	p->qnext = 0;
	p->nchild = 0;
	p->nwait = 0;
	p->waitq = 0;
	p->pgrp = 0;
	p->egrp = 0;
	p->fgrp = 0;
	p->rgrp = 0;
	p->pdbg = 0;
	p->fpstate = FPinit;
	p->kp = 0;
	p->procctl = 0;
	p->notepending = 0;
	p->mp = 0;
	p->movetime = 0;
	p->wired = 0;
	p->ureg = 0;
	memset(p->seg, 0, sizeof p->seg);
	p->pid = incref(&pidalloc);
	p->noteid = incref(&noteidalloc);
	if(p->pid==0 || p->noteid==0)
		panic("pidalloc");
	if(p->kstack == 0)
		p->kstack = smalloc(KSTACK);

	return p;
}

/*
 * wire this proc to a machine
 */
void
procwired(Proc *p)
{
	Proc *pp;
	int i, bm;
	char nwired[MAXMACH];

	memset(nwired, 0, sizeof(nwired));
	p->wired = 0;
	pp = proctab(0);
	for(i=0; i<conf.nproc; i++, pp++)
		if(pp->wired && pp->pid)
			nwired[pp->wired->machno]++;
	bm = 0;
	for(i=0; i<conf.nmach; i++)
		if(nwired[i] < nwired[bm])
			bm = i;
	p->wired = MACHP(bm);
	p->movetime = 0xffffffff;
	p->mp = p->wired;
}

void
procinit0(void)		/* bad planning - clashes with devproc.c */
{
	Proc *p;
	int i;

	procalloc.free = xalloc(conf.nproc*sizeof(Proc));
	procalloc.arena = procalloc.free;

	p = procalloc.free;
	for(i=0; i<conf.nproc-1; i++,p++)
		p->qnext = p+1;
	p->qnext = 0;
}

void
sleep1(Rendez *r, int (*f)(void*), void *arg)
{
	int s;

	/*
	 * spl is to allow lock to be called
	 * at interrupt time. lock is mutual exclusion
	 */
	s = splhi();
	up->r = r;	/* early so postnote knows */
	lock(r);

	/*
	 * if condition happened, never mind
	 */
	if((*f)(arg)){
		up->r = 0;
		unlock(r);
		splx(s);
		return;
	}

	/*
	 * now we are committed to
	 * change state and call scheduler
	 */
	if(r->p){
		print("double sleep %d %d\n", r->p->pid, up->pid);
		dumpstack();
	}
	up->state = Wakeme;
	r->p = up;
	unlock(r);
}

void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	int s;

	sleep1(r, f, arg);
	if(up->notepending == 0)
		sched();	/* notepending may go true while asleep */

	if(up->notepending) {
		up->notepending = 0;
		s = splhi();
		lock(r);
		if(r->p == up)
			r->p = 0;
		unlock(r);
		splx(s);
		error(Eintr);
	}
}

int
tfn(void *arg)
{
	return MACHP(0)->ticks >= up->twhen || (*up->tfn)(arg);
}

void
tsleep(Rendez *r, int (*fn)(void*), void *arg, int ms)
{
	ulong when;
	Proc *f, **l;

	when = MS2TK(ms)+MACHP(0)->ticks;

	lock(&talarm);
	/* take out of list if checkalarm didn't */
	if(up->trend) {
		l = &talarm.list;
		for(f = *l; f; f = f->tlink) {
			if(f == up) {
				*l = up->tlink;
				break;
			}
			l = &f->tlink;
		}
	}
	/* insert in increasing time order */
	l = &talarm.list;
	for(f = *l; f; f = f->tlink) {
		if(f->twhen >= when)
			break;
		l = &f->tlink;
	}
	up->trend = r;
	up->twhen = when;
	up->tfn = fn;
	up->tlink = *l;
	*l = up;
	unlock(&talarm);

	sleep(r, tfn, arg);
	up->twhen = 0;
}

/*
 * Expects that only one process can call wakeup for any given Rendez
 */
void
wakeup(Rendez *r)
{
	Proc *p;
	int s;

	s = splhi();
	lock(r);
	p = r->p;
	if(p){
		r->p = 0;
		if(p->state != Wakeme) 
			panic("wakeup: state");
		p->r = 0;
		ready(p);
	}
	unlock(r);
	splx(s);
}

int
postnote(Proc *p, int dolock, char *n, int flag)
{
	int s, ret;
	Rendez *r;
	Proc *d, **l;

	if(dolock)
		qlock(&p->debug);

	if(p->kp)
		print("sending %s to kproc %d %s\n", n, p->pid, p->text);

	if(flag != NUser && (p->notify == 0 || p->notified))
		p->nnote = 0;

	ret = 0;
	if(p->nnote < NNOTE) {
		strcpy(p->note[up->nnote].msg, n);
		p->note[p->nnote++].flag = flag;
		ret = 1;
	}
	p->notepending = 1;
	if(dolock)
		qunlock(&p->debug);

	r = p->r;
	if(r != 0) {
		for(;;) {
			s = splhi();
			if(canlock(r))
				break;
			splx(s);
		}
		/* check we won the race */
		if(p->r == r && r->p == p && p->state==Wakeme) {
			r->p = 0;
			p->r = 0;
			ready(p);
		}
		unlock(r);
		splx(s);
	}

	if(p->state != Rendezvous)
		return ret;

	/* Try and pull out of a rendezvous */
	lock(p->rgrp);
	if(p->state == Rendezvous) {
		p->rendval = ~0;
		l = &REND(p->rgrp, p->rendtag);
		for(d = *l; d; d = d->rendhash) {
			if(d == p) {
				*l = p->rendhash;
				break;
			}
			l = &d->rendhash;
		}
		ready(p);
	}
	unlock(p->rgrp);
	return ret;
}

/*
 * weird thing: keep at most NBROKEN around
 */
#define	NBROKEN 4
struct
{
	QLock;
	int	n;
	Proc	*p[NBROKEN];
}broken;

void
addbroken(Proc *p)
{
	qlock(&broken);
	if(broken.n == NBROKEN) {
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
		--broken.n;
	}
	broken.p[broken.n++] = p;
	qunlock(&broken);

	p->state = Broken;
	p->psstate = 0;
	sched();
}

void
unbreak(Proc *p)
{
	int b;

	qlock(&broken);
	for(b=0; b < broken.n; b++)
		if(broken.p[b] == p) {
			broken.n--;
			memmove(&broken.p[b], &broken.p[b+1],
					sizeof(Proc*)*(NBROKEN-(b+1)));
			ready(p);
			break;
		}
	qunlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	qlock(&broken);
	n = broken.n;
	for(i=0; i<n; i++) {
		ready(broken.p[i]);
		broken.p[i] = 0;
	}
	broken.n = 0;
	qunlock(&broken);
	return n;
}

void
pexit(char *exitstr, int freemem)
{
	int n;
	Proc *p;
	Segment **s, **es;
	long utime, stime;
	Waitq *wq, *f, *next;

	up->alarm = 0;

	if(up->fgrp)
		closefgrp(up->fgrp);
	if(up->egrp)
		closeegrp(up->egrp);
	if(up->rgrp)
		closergrp(up->rgrp);

	close(up->dot);
	closepgrp(up->pgrp);

	/*
	 * if not a kernel process and have a parent,
	 * do some housekeeping.
	 */
	if(up->kp == 0) {
		p = up->parent;
		if(p == 0) {
			if(exitstr == 0)
				exitstr = "unknown";
			panic("boot process died: %s", exitstr);
		}

		while(waserror())
			;
	
		wq = smalloc(sizeof(Waitq));
		poperror();

		readnum(0, wq->w.pid, NUMSIZE, up->pid, NUMSIZE);
		utime = up->time[TUser] + up->time[TCUser];
		stime = up->time[TSys] + up->time[TCSys];
		readnum(0, &wq->w.time[TUser*12], NUMSIZE,
			TK2MS(utime), NUMSIZE);
		readnum(0, &wq->w.time[TSys*12], NUMSIZE,
			TK2MS(stime), NUMSIZE);
		readnum(0, &wq->w.time[TReal*12], NUMSIZE,
			TK2MS(MACHP(0)->ticks - up->time[TReal]), NUMSIZE);
		if(exitstr && exitstr[0]){
			n = sprint(wq->w.msg, "%s %d:", up->text, up->pid);
			strncpy(wq->w.msg+n, exitstr, ERRLEN-n);
			wq->w.msg[ERRLEN-1] = 0;
		}
		else
			wq->w.msg[0] = '\0';

		lock(&p->exl);
		/* My parent still alive, processes are limited to 128
		 * Zombies to prevent a badly written daemon lots of wait
		 * records
		 */
		if(p->pid == up->parentpid && p->state != Broken && p->nwait < 128) {	
			p->nchild--;
			p->time[TCUser] += utime;
			p->time[TCSys] += stime;
	
			wq->next = p->waitq;
			p->waitq = wq;
			p->nwait++;
			unlock(&p->exl);
	
			wakeup(&p->waitr);
		}
		else {
			unlock(&p->exl);
			free(wq);
		}
	}

	if(!freemem)
		addbroken(up);

	es = &up->seg[NSEG];
	for(s = up->seg; s < es; s++) {
		if(*s) {
			putseg(*s);
			*s = 0;
		}
	}

	lock(&up->exl);		/* Prevent my children from leaving waits */
	up->pid = 0;
	unlock(&up->exl);

	for(f = up->waitq; f; f = next) {
		next = f->next;
		free(f);
	}

	/* release debuggers */
	qlock(&up->debug);
	if(up->pdbg) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = 0;
	}
	qunlock(&up->debug);

	/* Sched must not loop for this lock */
	lock(&procalloc);

	up->state = Moribund;
	sched();
	panic("pexit");
}

int
haswaitq(void *x)
{
	Proc *p;

	p = (Proc *)x;
	return p->waitq != 0;
}

ulong
pwait(Waitmsg *w)
{
	ulong cpid;
	Waitq *wq;

	if(!canqlock(&up->qwaitr))
		error(Einuse);

	if(waserror()) {
		qunlock(&up->qwaitr);
		nexterror();
	}

	lock(&up->exl);
	if(up->nchild == 0 && up->waitq == 0) {
		unlock(&up->exl);
		error(Enochild);
	}
	unlock(&up->exl);

	sleep(&up->waitr, haswaitq, up);

	lock(&up->exl);
	wq = up->waitq;
	up->waitq = wq->next;
	up->nwait--;
	unlock(&up->exl);

	qunlock(&up->qwaitr);
	poperror();

	if(w)
		memmove(w, &wq->w, sizeof(Waitmsg));
	cpid = atoi(wq->w.pid);
	free(wq);
	return cpid;
}

Proc*
proctab(int i)
{
	return &procalloc.arena[i];
}

void
dumpaproc(Proc *p)
{
	ulong bss;
	char *s;

	if(p == 0)
		return;

	bss = 0;
	if(p->seg[BSEG])
		bss = p->seg[BSEG]->top;

	s = p->psstate;
	if(s == 0)
		s = "kproc";
	print("%3d:%10s pc %8lux dbgpc %8lux  %8s (%s) ut %ld st %ld bss %lux\n",
		p->pid, p->text, p->pc, dbgpc(p),  s, statename[p->state],
		p->time[0], p->time[1], bss, p->priority);
}

void
procdump(void)
{
	int i;
	Proc *p;

	if(up)
		print("up %d\n", up->pid);
	else
		print("no current process\n");
	for(i=0; i<conf.nproc; i++) {
		p = &procalloc.arena[i];
		if(p->state == Dead)
			continue;

		dumpaproc(p);
		delay(150);
	}
}

void
scheddump(void)
{
	Proc *p;
	Schedq *rq;

	for(rq = &runq[Nrq-1]; rq >= runq; rq--){
		if(rq->head == 0)
			continue;
		print("rq%d:", rq-runq);
		for(p = rq->head; p; p = p->rnext)
			print(" %d(%d, %d)", p->pid, m->ticks - p->readytime,
				m->ticks - p->movetime);
		print("\n");
		delay(150);
	}
	print("nrdy %d\n", nrdy);
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	static Pgrp *kpgrp;

	p = newproc();
	p->psstate = 0;
	p->procmode = 0644;
	p->kp = 1;

	p->fpsave = up->fpsave;
	p->scallnr = up->scallnr;
	p->s = up->s;
	p->nerrlab = 0;
	p->slash = up->slash;
	p->dot = up->dot;
	incref(p->dot);

	memmove(p->note, up->note, sizeof(p->note));
	p->nnote = up->nnote;
	p->notified = 0;
	p->lastnote = up->lastnote;
	p->notify = up->notify;
	p->ureg = 0;
	p->dbgreg = 0;

	p->basepri = PriKproc;
	p->priority = p->basepri;

	kprocchild(p, func, arg);

	strcpy(p->user, eve);
	if(kpgrp == 0)
		kpgrp = newpgrp();
	p->pgrp = kpgrp;
	incref(kpgrp);

	strcpy(p->text, name);

	p->nchild = 0;
	p->parent = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	ready(p);
	/*
	 *  since the bss/data segments are now shareable,
	 *  any mmu info about this process is now stale
	 *  and has to be discarded.
	 */
	flushmmu();
}

/*
 *  called splhi() by notify().  See comment in notify for the
 *  reasoning.
 */
void
procctl(Proc *p)
{
	char *state;
	ulong s;

	switch(p->procctl) {
	case Proc_exitbig:
		spllo();
		pexit("Killed: Insufficient physical memory", 1);

	case Proc_exitme:
		spllo();		/* pexit has locks in it */
		pexit("Killed", 1);

	case Proc_traceme:
		if(p->nnote == 0)
			return;
		/* No break */

	case Proc_stopme:
		p->procctl = 0;
		state = p->psstate;
		p->psstate = "Stopped";
		/* free a waiting debugger */
		s = spllo();
		qlock(&p->debug);
		if(p->pdbg) {
			wakeup(&p->pdbg->sleep);
			p->pdbg = 0;
		}
		qunlock(&p->debug);
		splhi();
		p->state = Stopped;
		sched();
		p->psstate = state;
		splx(s);
		return;
	}
}

#include "errstr.h"

void
error(char *err)
{
	spllo();
	strncpy(up->error, err, ERRLEN);
	nexterror();
}

void
nexterror(void)
{
	gotolabel(&up->errlab[--up->nerrlab]);
}

void
exhausted(char *resource)
{
	char buf[ERRLEN];

	sprint(buf, "no free %s", resource);
	error(buf);
}

void
killbig(void)
{
	int i;
	Segment *s;
	ulong l, max;
	Proc *p, *ep, *kp;

	max = 0;
	kp = 0;
	ep = procalloc.arena+conf.nproc;
	for(p = procalloc.arena; p < ep; p++) {
		if(p->state == Dead || p->kp)
			continue;
		l = 0;
		for(i=1; i<NSEG; i++) {
			s = p->seg[i];
			if(s != 0)
				l += s->top - s->base;
		}
		if(l > max) {
			kp = p;
			max = l;
		}
	}
	kp->procctl = Proc_exitbig;
	for(i = 0; i < NSEG; i++) {
		s = kp->seg[i];
		if(s != 0 && canqlock(&s->lk)) {
			mfreeseg(s, s->base, (s->top - s->base)/BY2PG);
			qunlock(&s->lk);
		}
	}
	print("%d: %s killed because no swap configured\n", kp->pid, kp->text);
}

/*
 *  change ownership to 'new' of all processes owned by 'old'.  Used when
 *  eve changes.
 */
void
renameuser(char *old, char *new)
{
	Proc *p, *ep;

	ep = procalloc.arena+conf.nproc;
	for(p = procalloc.arena; p < ep; p++)
		if(strcmp(old, p->user) == 0)
			memmove(p->user, new, NAMELEN);
}

/*
 *  time accounting called by clock() splhi'd
 */
void
accounttime(void)
{
	Proc *p;
	int n;
	static int nrun;

	p = m->proc;
	if(p) {
		nrun++;
		p->time[p->insyscall]++;
	}

	/* only one processor gets to compute system load averages */
	if(m->machno != 0)
		return;

	/* calculate decaying load average */
	n = nrun;
	nrun = 0;

	n = (nrdy+n)*1000;
	m->load = (m->load*19+n)/20;
}
