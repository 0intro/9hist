#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

struct
{
	Lock;
	ulong	pid;
}pidalloc;

struct
{
	Lock;
	Proc	*arena;
	Proc	*free;
}procalloc;

typedef struct
{
	Lock;
	Proc	*head;
	Proc	*tail;
}Schedq;

Schedq runhiq, runloq;

char *statename[]={	/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"Zombie",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"MMUing",
	"Exiting",
	"Inwait",
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
	Proc *p;
	Page *pg;

	setlabel(&m->sched);
	if(u){
		m->proc = 0;
		p = u->p;
		invalidateu();	/* safety first */
		u = 0;
		if(p->state == Running)
			ready(p);
		else if(p->state == Moribund) {
			/*
			 * The Grim Reaper lays waste the bodies of the dead
			 */
			p->pid = 0;
			mmurelease(p);
			/* 
			 * Holding locks from pexit:
			 * 	procalloc, debug, palloc
			 */
			pg = p->upage;
			pg->ref = 0;
			p->upage = 0;
			palloc.freecount++;
			if(palloc.head) {
				pg->next = palloc.head;
				palloc.head->prev = pg;
				pg->prev = 0;
				palloc.head = pg;
			}
			else {
				palloc.head = palloc.tail = pg;
				pg->prev = pg->next = 0;
			}

			p->qnext = procalloc.free;
			procalloc.free = p;
		
			unlock(&palloc);
			unlock(&p->debug);
			unlock(&procalloc);

			p->state = Dead;
		}
		p->mach = 0;
	}
	sched();
}

void
sched(void)
{
	uchar procstate[64];
	Proc *p;
	ulong tlbvirt, tlbphys;
	void (*f)(ulong, ulong);

	if(u){
		splhi();
		m->cs++;
		procsave(procstate, sizeof(procstate));
		if(setlabel(&u->p->sched)){	/* woke up */
			p = u->p;
			p->state = Running;
			p->mach = m;
			m->proc = p;
			procrestore(p, procstate);
			spllo();
			return;
		}
		gotolabel(&m->sched);
	}
	spllo();
	p = runproc();
	splhi();
	mapstack(p);
	gotolabel(&p->sched);
}

int
anyready(void)
{
	return runloq.head != 0 || runhiq.head != 0;
}

void
ready(Proc *p)
{
	Schedq *rq;
	int s;

	s = splhi();

	if(p->state == Running)
		rq = &runloq;
	else
		rq = &runhiq;

	lock(&runhiq);
	p->rnext = 0;
	if(rq->tail)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;

	p->state = Ready;
	unlock(&runhiq);
	splx(s);
}

/*
 * Always called spllo
 */
Proc*
runproc(void)
{
	Schedq *rq;
	Proc *p;
	int i;

loop:
	while(runhiq.head==0 && runloq.head==0)
		for(i=0; i<10; i++)	/* keep out of shared memory for a while */
			;
	splhi();
	lock(&runhiq);
	if(runhiq.head)
		rq = &runhiq;
	else
		rq = &runloq;

	p = rq->head;
	if(p==0 || p->mach){	/* p->mach==0 only when process state is saved */
		unlock(&runhiq);
		spllo();
		goto loop;
	}
	if(p->rnext == 0)
		rq->tail = 0;
	rq->head = p->rnext;
	if(p->state != Ready)
		print("runproc %s %d %s\n", p->text, p->pid, statename[p->state]);
	unlock(&runhiq);
	p->state = Scheding;
	spllo();
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

loop:
	lock(&procalloc);
	if(p = procalloc.free){		/* assign = */
		procalloc.free = p->qnext;
		p->state = Zombie;
		unlock(&procalloc);
		p->mach = 0;
		p->qnext = 0;
		p->nchild = 0;
		p->child = 0;
		p->exiting = 0;
		p->pgrp = 0;
		p->egrp = 0;
		p->fgrp = 0;
		p->fpstate = FPinit;
		p->kp = 0;
		p->procctl = 0;
		p->notepending = 0;
		memset(p->seg, 0, sizeof p->seg);
		lock(&pidalloc);
		p->pid = ++pidalloc.pid;
		unlock(&pidalloc);
		if(p->pid == 0)
			panic("pidalloc");
		return p;
	}
	unlock(&procalloc);
	print("no procs\n");
	if(u == 0)
		panic("newproc");
	u->p->state = Wakeme;
	alarm(1000, wakeme, u->p);
	sched();
	goto loop;
}

void
procinit0(void)		/* bad planning - clashes with devproc.c */
{
	Proc *p;
	int i;

	procalloc.free = ialloc(conf.nproc*sizeof(Proc), 0);
	procalloc.arena = procalloc.free;

	p = procalloc.free;
	for(i=0; i<conf.nproc-1; i++,p++)
		p->qnext = p+1;
	p->qnext = 0;
}

void
sleep1(Rendez *r, int (*f)(void*), void *arg)
{
	Proc *p;
	int s;

	/*
	 * spl is to allow lock to be called
	 * at interrupt time. lock is mutual exclusion
	 */
	s = splhi();
	p = u->p;
	p->r = r;	/* early so postnote knows */
	lock(r);

	/*
	 * if condition happened, never mind
	 */
	if((*f)(arg)){
		p->r = 0;
		unlock(r);
		splx(s);
		return;
	}

	/*
	 * now we are committed to
	 * change state and call scheduler
	 */
	if(r->p){
		print("double sleep %d %d\n", r->p->pid, p->pid);
		dumpstack();
	}
	p->state = Wakeme;
	r->p = p;
	unlock(r);
}

void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	Proc *p;

	p = u->p;
	sleep1(r, f, arg);
	if(p->notepending == 0)
		sched();	/* notepending may go true while asleep */
	if(p->notepending){
		p->notepending = 0;
		lock(r);
		if(r->p == p)
			r->p = 0;
		unlock(r);
		error(Eintr);
	}
}

void
tsleep(Rendez *r, int (*f)(void*), void *arg, int ms)
{
	Alarm *a;
	Proc *p;

	p = u->p;
	sleep1(r, f, arg);
	if(p->notepending == 0){
		a = alarm(ms, twakeme, r);
		sched();	/* notepending may go true while asleep */
		cancel(a);
	}
	if(p->notepending){
		p->notepending = 0;
		lock(r);
		if(r->p == p)
			r->p = 0;
		unlock(r);
		error(Eintr);
	}
}

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

void
wakeme(Alarm *a)
{
	ready((Proc*)(a->arg));
	cancel(a);
}

void
twakeme(Alarm *a)
{
	wakeup((Rendez*)(a->arg));
}

int
postnote(Proc *p, int dolock, char *n, int flag)
{
	User *up;
	KMap *k;
	int s;
	Rendez *r;
	Proc *d, **l;

	SET(k);
	USED(k);

	if(dolock)
		lock(&p->debug);

	if(p->upage == 0){
		if(dolock)
			unlock(&p->debug);
		errors("noted process disappeared");
	}

	if(u == 0 || p != u->p){
		k = kmap(p->upage);
		up = (User*)VA(k);
	}else 
		up = u;


	if(flag!=NUser && (up->notify==0 || up->notified))
		up->nnote = 0;	/* force user's hand */
	else if(up->nnote == NNOTE-1){
		if(up != u)
			kunmap(k);
		if(dolock)
			unlock(&p->debug);
		return 0;
	}
	p->notepending = 1;
	strcpy(up->note[up->nnote].msg, n);
	up->note[up->nnote++].flag = flag;
	if(up != u)
		kunmap(k);
	if(dolock)
		unlock(&p->debug);

	if(r = p->r){		/* assign = */
		/* wake up; can't call wakeup itself because we're racing with it */
		s = splhi();
		lock(r);
		if(p->r==r && r->p==p){	/* check we won the race */
			if(p->state == Wakeme){
				r->p = 0;
				p->r = 0;
				ready(p);
			}
		}
		unlock(r);
		splx(s);
	}

	if(p->state == Rendezvous) {
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
	}

	return 1;
}


/*
 * weird thing: keep at most NBROKEN around
 */
#define	NBROKEN 4
struct{
	Lock;
	int	n;
	Proc	*p[NBROKEN];
}broken;

void
addbroken(Proc *c)
{
	int b;

	lock(&broken);
	if(broken.n == NBROKEN){
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
		--broken.n;
	}
	broken.p[broken.n++] = c;
	unlock(&broken);
	c->state = Broken;
	sched();		/* until someone lets us go */
	lock(&broken);
	for(b=0; b<NBROKEN; b++)
		if(broken.p[b] == c){
			broken.n--;
			memmove(&broken.p[b], &broken.p[b+1], sizeof(Proc*)*(NBROKEN-(b+1)));
			break;
		}
	unlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	lock(&broken);
	n = broken.n;
	for(i=0; i<n; i++)
		ready(broken.p[i]);
	broken.n = 0;
	unlock(&broken);
	return n;
}

void
pexit(char *exitstr, int freemem)
{
	ulong mypid;
	Proc *p, *c, *k, *l;
	int n, i;
	Chan *ch;
	char msg[ERRLEN];
	ulong *up, *ucp, *wp;
	Segment **s, **es, *os;

	if(exitstr) 			/* squirrel away before we lose our address space */
		strcpy(msg, exitstr);
	else
		msg[0] = 0;
	
	c = u->p;
	c->alarm = 0;
	mypid = c->pid;

	if(c->fgrp)
		closefgrp(c->fgrp);

	if(freemem){
		flushvirt();
		es = &c->seg[NSEG];
		for(s = c->seg; s < es; s++)
			if(os = *s) {
				*s = 0;
				putseg(os);
			}
		closepgrp(c->pgrp);
		if(c->egrp)
			closeegrp(c->egrp);
		close(u->dot);
	}
	/*
	 * Any of my children exiting?
	 */
	while(c->nchild){
		lock(&c->wait.queue);
		if(canlock(&c->wait.use)){	/* no child is exiting */
			c->exiting = 1;
			unlock(&c->wait.use);
			unlock(&c->wait.queue);
			break;
		}else{				/* must wait for child */
			unlock(&c->wait.queue);
			pwait(0);
		}
	}

	c->time[TReal] = MACHP(0)->ticks - c->time[TReal];
	/*
	 * Tell my parent
	 */
	p = c->parent;
	if(p == 0)
		goto out;
	qlock(&p->wait);
	lock(&p->wait.queue);
	if(p->pid==c->parentpid && !p->exiting){
		memmove(p->waitmsg.msg, msg, ERRLEN);
		p->waitmsg.pid = mypid;
		wp = &p->waitmsg.time[TUser];
		up = &c->time[TUser];
		ucp = &c->time[TCUser];
		*wp++ = TK2MS(*up++ + *ucp++);
		*wp++ = TK2MS(*up++ + *ucp  );
		*wp   = TK2MS(*up           );
		p->child = c;
		c->state = Exiting;
		if(p->state == Inwait)
			ready(p);
		unlock(&p->wait.queue);
		sched();
	}else{
		unlock(&p->wait.queue);
		qunlock(&p->wait);
	}
   out:
	if(!freemem){
		addbroken(c);
		flushvirt();
		es = &c->seg[NSEG];
		for(s = c->seg; s < es; s++)
			if(os = *s) {
				*s = 0;
				putseg(os);
			}
		closepgrp(c->pgrp);
		closeegrp(c->egrp);
		close(u->dot);
	}

    done:

	/*
	 * sched() cannot wait on these locks
	 */
	lock(&procalloc);
	lock(&c->debug);
	lock(&palloc);

	c->state = Moribund;
	sched();
	panic("pexit");
}

ulong
pwait(Waitmsg *w)
{
	Proc *c, *p;
	ulong cpid;

	p = u->p;
again:
	while(canlock(&p->wait.use)){
		if(p->nchild == 0){
			qunlock(&p->wait);
			error(Enochild);
		}
		p->state = Inwait;
		qunlock(&p->wait);
		sched();
	}
	lock(&p->wait.queue);	/* wait until child is finished */
	c = p->child;
	if(c == 0){
		p->state = Inwait;
		unlock(&p->wait.queue);
		sched();
		goto again;
	}
	p->child = 0;
	if(w)
		*w = p->waitmsg;
	cpid = p->waitmsg.pid;
	p->time[TCUser] += c->time[TUser] + c->time[TCUser];
	p->time[TCSys] += c->time[TSys] + c->time[TCSys];
	p->time[TCReal] += c->time[TReal];
	p->nchild--;
	unlock(&p->wait.queue);
	qunlock(&p->wait);
	ready(c);
	return cpid;
}


Proc*
proctab(int i)
{
	return &procalloc.arena[i];
}

#include <ureg.h>
void
procdump(void)
{
	int i;
	Proc *p;

	for(i=0; i<conf.nproc; i++){
		p = procalloc.arena+i;
		if(p->state != Dead){
			print("%d:%s %s upc %lux %s ut %ld st %ld r %lux qpc %lux\n",
				p->pid, p->text, 
				p->pgrp ? p->pgrp->user : "pgrp=0", p->pc, 
				statename[p->state], p->time[0], p->time[1], p->r, p->qlockpc);
		}
	}
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	int n;
	ulong upa;
	int lastvar;	/* used to compute stack address */
	User *up;
	KMap *k;
	static Pgrp *kpgrp;
	char *user;

	/*
	 * Kernel stack
	 */
	p = newproc();
	p->kp = 1;
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));
	k = kmap(p->upage);
	upa = VA(k);
	up = (User*)upa;

	/*
	 * Save time: only copy u-> data and useful stack
	 */
	clearmmucache();	/* so child doesn't inherit any of your mappings */
	memmove(up, u, sizeof(User));
	n = USERADDR+BY2PG - (ulong)&lastvar;
	n = (n+32) & ~(BY2WD-1);	/* be safe & word align */
	memmove((void*)(upa+BY2PG-n), (void*)(USERADDR+BY2PG-n), n);
	up->p = p;

	/*
	 * Refs
	 */
	incref(up->dot);
	kunmap(k);

	/*
	 * Sched
	 */
	if(setlabel(&p->sched)){
		/*
		 *  use u->p instead of p, because we
		 *  don't trust the compiler, after a
		 *  gotolabel, to find the correct contents
		 *  of a local variable.  Passed parameters
		 *  (func & arg) are a bit safer since we
		 *  don't play with them anywhere else.
		 */
		p = u->p;
		p->state = Running;
		p->mach = m;
		m->proc = p;
		spllo();
		(*func)(arg);
		pexit(0, 1);
	}

	user = "bootes";
	if(kpgrp == 0){
		kpgrp = newpgrp();
		strcpy(kpgrp->user, user);
	}
	p->pgrp = kpgrp;
	incref(kpgrp);

	if(u->p->pgrp->user[0] != '\0')
		user = u->p->pgrp->user;
	sprint(p->text, "%s.%.6s", name, user);

	p->nchild = 0;
	p->parent = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	/*
	 *  since the bss/data segments are now shareable,
	 *  any mmu info about this process is now stale
	 *  and has to be discarded.
	 */
	flushmmu();
	clearmmucache();
	ready(p);
}

void
procctl(Proc *p)
{
	switch(p->procctl) {
	case Proc_exitme:
		pexit("Killed", 1);
	case Proc_stopme:
		p->procctl = 0;
		p->state = Stopped;
		sched();
		return;
	}
}

#include "errstr.h"

void
error(int code)
{
	strncpy(u->error, errstrtab[code], ERRLEN);
	nexterror();
}

void
errors(char *err)
{
	strncpy(u->error, err, ERRLEN);
	nexterror();
}

void
nexterror(void)
{
	gotolabel(&u->errlab[--u->nerrlab]);
}

