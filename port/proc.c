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

struct
{
	Lock;
	Proc	*head;
	Proc	*tail;
}runq;

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
};

/*
 * Always splhi()'ed.
 */
void
schedinit(void)		/* never returns */
{
	Proc *p;

	setlabel(&m->sched);
	if(u){
		m->proc = 0;
		p = u->p;
		invalidateu();	/* safety first */
		u = 0;
		if(p->state == Running)
			ready(p);
		else if(p->state == Moribund){
			p->pid = 0;
			unlock(&p->debug);
			p->upage->ref--;
			/* procalloc already locked */
			p->qnext = procalloc.free;
			procalloc.free = p;
			p->upage = 0;
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
	uchar procstate[64];		/* sleeze for portability */
	Proc *p;
	ulong tlbvirt, tlbphys;
	void (*f)(ulong, ulong);

	if(u){
		splhi();
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
	return runq.head != 0;
}

void
ready(Proc *p)
{
	int s;

	s = splhi();
	lock(&runq);
	p->rnext = 0;
	if(runq.tail)
		runq.tail->rnext = p;
	else
		runq.head = p;
	runq.tail = p;
	p->state = Ready;
	unlock(&runq);
	splx(s);
}

/*
 * Always called spllo
 */
Proc*
runproc(void)
{
	Proc *p;
	int i;

loop:
	while(runq.head == 0)
		for(i=0; i<10; i++)	/* keep out of shared memory for a while */
			;
	splhi();
	lock(&runq);
	p = runq.head;
	if(p==0 || p->mach){	/* p->mach==0 only when process state is saved */
		unlock(&runq);
		spllo();
		goto loop;
	}
	if(p->rnext == 0)
		runq.tail = 0;
	runq.head = p->rnext;
	if(p->state != Ready)
		print("runproc %s %d %s\n", p->text, p->pid, statename[p->state]);
	unlock(&runq);
	p->state = Scheding;
	spllo();
	return p;
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
		p->fpstate = FPinit;
		memset(p->pidonmach, 0, sizeof p->pidonmach);
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
	lock(r);

	/*
	 * if condition happened, never mind
	 */
	if((*f)(arg)){	
		unlock(r);
		splx(s);
		return;
	}

	/*
	 * now we are committed to
	 * change state and call scheduler
	 */
	p = u->p;
	if(r->p)
		print("double sleep %d %d\n", r->p->pid, p->pid);
	p->r = r;
	p->wokeup = 0;
	p->state = Wakeme;
	r->p = p;
	unlock(r);
}

void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	sleep1(r, f, arg);
	sched();
	if(u->p->wokeup){
		u->p->wokeup = 0;
		error(Eintr);
	}
}

void
tsleep(Rendez *r, int (*f)(void*), void *arg, int ms)
{
	Alarm *a;

	sleep1(r, f, arg);
	a = alarm(ms, twakeme, r);
	sched();
	cancel(a);
	if(u->p->wokeup){
		u->p->wokeup = 0;
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
			panic("wakeup: not Wakeme");
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

	if(dolock)
		lock(&p->debug);
	if(p != u->p){
		k = kmap(p->upage);
		up = (User*)VA(k);
	}else
		up = u;
	if(flag!=NUser && (up->notify==0 || up->notified))
		up->nnote = 0;	/* force user's hand */
	else if(up->nnote == NNOTE-1){
		if(up != u)
			kunmap(k);
		return 0;
	}
	strcpy(up->note[up->nnote].msg, n);
	up->note[up->nnote++].flag = flag;
	if(up != u)
		kunmap(k);
	if(dolock)
		unlock(&p->debug);
	if(r = p->r){	/* assign = */
		/* wake up */
		s = splhi();
		lock(r);
		if(p->r==r && r->p==p){
			r->p = 0;
			if(p->state != Wakeme)
				panic("postnote wakeup: not Wakeme");
			p->wokeup = 1;
			p->r = 0;
			ready(p);
		}
		unlock(r);
		splx(s);
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
		memcpy(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
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
			memcpy(&broken.p[b], &broken.p[b+1], sizeof(Proc*)*(NBROKEN-(b+1)));
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
pexit(char *s, int freemem)
{
	ulong mypid;
	Proc *p, *c, *k, *l;
	int n;
	Chan *ch;
	char msg[ERRLEN];
	ulong *up, *ucp, *wp;

	if(s)	/* squirrel away; we'll lose our address space */
		strcpy(msg, s);
	else
		msg[0] = 0;
	c = u->p;
	mypid = c->pid;
	if(freemem){
		freesegs(-1);
		closepgrp(c->pgrp);
		close(u->dot);
	}
	for(n=0; n<=u->maxfd; n++)
		if(ch = u->fd[n])	/* assign = */
			close(ch);
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
		memcpy(p->waitmsg.msg, msg, ERRLEN);
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
		freesegs(-1);
		closepgrp(c->pgrp);
		close(u->dot);
	}

    done:

	lock(&procalloc);	/* sched() can't do this */
	lock(&c->debug);	/* sched() can't do this */
	unusepage(c->upage, 1);	/* sched() can't do this (it locks) */
	c->state = Moribund;
	sched();	/* never returns */
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
DEBUG(void)
{
	int i;
	Proc *p;
	Orig *o;

	print("DEBUG\n");
	for(i=0; i<conf.nproc; i++){
		p = procalloc.arena+i;
		if(p->state != Dead){
			print("%d:%s %s upc %lux %s ut %ld st %ld r %lux\n",
				p->pid, p->text, p->pgrp->user, p->pc, statename[p->state],
				p->time[0], p->time[1], p->r);
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

	/*
	 * Kernel stack
	 */
	p = newproc();
	p->kp = 1;
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));
	k = kmap(p->upage);
	upa = VA(k);
	up = (User*)upa;
	up->p = p;

	/*
	 * Save time: only copy u-> data and useful stack
	 */
	clearmmucache();
	memcpy(up, u, sizeof(User));
	n = USERADDR+BY2PG - (ulong)&lastvar;
	n = (n+32) & ~(BY2WD-1);	/* be safe & word align */
	memcpy((void*)(upa+BY2PG-n), (void*)(USERADDR+BY2PG-n), n);

	/*
	 * Refs
	 */
	incref(up->dot);
	for(n=0; n<=up->maxfd; n++)
		up->fd[n] = 0;
	up->maxfd = 0;
	kunmap(k);

	/*
	 * Sched
	 */
	if(setlabel(&p->sched)){
		u->p = p;
		p->state = Running;
		p->mach = m;
		m->proc = p;
		spllo();
		(*func)(arg);
		pexit(0, 1);
	}
	if(kpgrp == 0){
		kpgrp = newpgrp();
		strcpy(kpgrp->user, "bootes");
	}
	p->pgrp = kpgrp;
	incref(kpgrp);
	sprint(p->text, "%s.%.6s", name, u->p->pgrp->user);
	p->nchild = 0;
	p->parent = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	ready(p);
	flushmmu();
}
