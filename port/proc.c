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
}Schedq;

Schedq	runhiq, runloq;
int	nrdy;

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
			up->pid = 0;
			up->state = Dead;
			/* 
			 * Holding locks from pexit:
			 * 	procalloc, debug, palloc
			 */
			mmurelease(up);

			up->qnext = procalloc.free;
			procalloc.free = up;
		
			unlock(&palloc);
			qunlock(&up->debug);
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
	up->mach = m;
	m->proc = up;
	mmuswitch(up);
	gotolabel(&up->sched);
}

int
anyready(void)
{
	return runloq.head != 0 || runhiq.head != 0;
}

void
ready(Proc *p)
{
	int s;
	Schedq *rq;

	s = splhi();

	rq = &runhiq;
	if(p->state == Running)
		rq = &runloq;

	lock(&runhiq);
	p->rnext = 0;
	if(rq->tail)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;

	nrdy++;
	p->state = Ready;
	unlock(&runhiq);
	splx(s);
}

Proc*
runproc(void)
{
	Schedq *rq;
	Proc *p;

loop:
	spllo();
	while(runhiq.head == 0 && runloq.head == 0)
		;
	splhi();

	lock(&runhiq);
	if(runhiq.head)
		rq = &runhiq;
	else
		rq = &runloq;

	p = rq->head;
	/* p->mach==0 only when process state is saved */
	if(p == 0 || p->mach){	
		unlock(&runhiq);
		/* keep off the bus (tuned for everest) */
		if(conf.nmach > 1)
			delay(7);
		goto loop;
	}
	if(p->rnext == 0)
		rq->tail = 0;
	rq->head = p->rnext;
	nrdy--;
	if(p->state != Ready)
		print("runproc %s %d %s\n", p->text, p->pid, statename[p->state]);
	unlock(&runhiq);
	p->state = Scheding;
	return p;
}

int
canpage(Proc *p)
{
	int ok = 0;

	splhi();
	lock(&runhiq);
	/* Only reliable way to see if we are Running */
	if(p->mach == 0) {
		p->newtlb = 1;
		ok = 1;
	}
	unlock(&runhiq);
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
	p->pdbg = 0;
	p->fpstate = FPinit;
	p->kp = 0;
	p->procctl = 0;
	p->notepending = 0;
	memset(p->seg, 0, sizeof p->seg);
	p->pid = incref(&pidalloc);
	p->noteid = incref(&noteidalloc);
	if(p->pid==0 || p->noteid==0)
		panic("pidalloc");
	if(p->kstack == 0)
		p->kstack = smalloc(KSTACK);

	return p;
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

	if(r = p->r) {
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
	lock(p->pgrp);
	if(p->state == Rendezvous) {
		p->rendval = ~0;
		l = &REND(p->pgrp, p->rendtag);
		for(d = *l; d; d = d->rendhash) {
			if(d == p) {
				*l = p->rendhash;
				break;
			}
			l = &d->rendhash;
		}
		ready(p);
	}
	unlock(p->pgrp);
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
	for(s = up->seg; s < es; s++)
		if(*s)
			putseg(*s);

	lock(&up->exl);		/* Prevent my children from leaving waits */
	up->pid = 0;
	unlock(&up->exl);

	for(f = up->waitq; f; f = next) {
		next = f->next;
		free(f);
	}

	/*
	 * sched() cannot wait on these locks
	 */
	qlock(&up->debug);
	/* release debuggers */
	if(up->pdbg) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = 0;
	}

	lock(&procalloc);
	lock(&palloc);

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
procdump(void)
{
	int i;
	char *s;
	Proc *p;
	ulong bss;

	for(i=0; i<conf.nproc; i++) {
		p = &procalloc.arena[i];
		if(p->state == Dead)
			continue;
		bss = 0;
		if(p->seg[BSEG])
			bss = p->seg[BSEG]->top;

		s = p->psstate;
		if(s == 0)
			s = "kproc";
		print("%3d:%10s pc %8lux %8s (%s) ut %ld st %ld bss %lux\n",
			p->pid, p->text, p->pc,  s, statename[p->state],
			p->time[0], p->time[1], bss);
	}
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

	switch(p->procctl) {
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
		qlock(&p->debug);
		if(p->pdbg) {
			wakeup(&p->pdbg->sleep);
			p->pdbg = 0;
		}
		qunlock(&p->debug);
		p->state = Stopped;
		sched();		/* sched returns spllo() */
		splhi();
		p->psstate = state;
		return;
	}
}

#include "errstr.h"

void
error(char *err)
{
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
