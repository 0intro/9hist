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
		putkmmu(USERADDR, INVALIDPTE);	/* safety first */
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
	Proc *p;
	ulong tlbvirt, tlbphys;
	void (*f)(ulong);

	if(u){
		splhi();
		if(setlabel(&u->p->sched)){	/* woke up */
if(u->p->mach)panic("mach non zero");
			p = u->p;
			p->state = Running;
			p->mach = m;
			m->proc = p;
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

void
checksched(void)		/* just for efficiency; don't sched if no need */
{
	if(runq.head)
		sched();
}

/*
 * Always called spllo
 */
Proc*
runproc(void)
{
	Proc *p;

loop:
	do; while(runq.head == 0);
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
		p->kid = 0;
		p->sib = 0;
		p->pop = 0;
		p->nchild = 0;
		p->child = 0;
		p->exiting = 0;
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
		error(0, Eintr);
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
		error(0, Eintr);
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
	int s;
	Rendez *r;

	if(dolock)
		lock(&p->debug);
	up = (User*)(p->upage->pa|KZERO);
	if(flag!=NUser && (up->notify==0 || up->notified))
		up->nnote = 0;	/* force user's hand */
	else if(up->nnote == NNOTE-1)
		return 0;
	strcpy(up->note[up->nnote].msg, n);
	up->note[up->nnote++].flag = flag;
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

void
pexit(char *s, int freemem)
{
	char status[64];
	ulong mypid;
	Proc *p, *c, *k, *l;
	Waitmsg w;
	int n;
	Chan *ch;
	ulong *up, *ucp, *wp;

	c = u->p;
	mypid = c->pid;
	if(s)
		strcpy(status, s);
	else
		status[0] = 0;
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
		w.pid = mypid;
		strcpy(w.msg, status);
		wp = &w.time[TUser];
		up = &c->time[TUser];
		ucp = &c->time[TCUser];
		*wp++ = (*up++ + *ucp++)*MS2HZ;
		*wp++ = (*up++ + *ucp  )*MS2HZ;
		*wp   = (*up           )*MS2HZ;
		p->child = c;
		/*
		 * Pass info through back door, to avoid huge Proc's
		 */
		p->waitmsg = (Waitmsg*)(c->upage->pa|(((ulong)&w)&(BY2PG-1))|KZERO);
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
		/*
		 * weird thing: keep at most NBROKEN around
		 */
		#define	NBROKEN 4
		static struct{
			Lock;
			int	n;
			Proc	*p[NBROKEN];
		}broken;
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
		freesegs(-1);
		closepgrp(c->pgrp);
		close(u->dot);
	}

	/*
	 * Rearrange inheritance hierarchy
	 * 1. my children's pop is now my pop
	 */
	lock(&c->kidlock);
	p = c->pop;
	if(k = c->kid)		/* assign = */
		do{
			k->pop = p;
			k = k->sib;
		}while(k != c->kid);

	/*
	 * 2. cut me from pop's tree
	 */
	if(p == 0)	/* init process only; fix pops */
		goto done;
	lock(&p->kidlock);
	k = p->kid;
	while(k->sib != c)
		k = k->sib;
	if(k == c)
		p->kid = 0;
	else{
		if(p->kid == c)
			p->kid = c->sib;
		k->sib = c->sib;
	}

	/*
	 * 3. pass my children (pop's grandchildren) to pop
	 */
	if(k = c->kid){		/* assign = */
		if(p->kid == 0)
			p->kid = k;
		else{
			l = k->sib;
			k->sib = p->kid->sib;
			p->kid->sib = l;
		}
	}
	unlock(&p->kidlock);
    done:
	unlock(&c->kidlock);

	lock(&procalloc);	/* sched() can't do this */
	lock(&c->debug);	/* sched() can't do this */
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
			error(0, Enochild);
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
		*w = *p->waitmsg;
	cpid = p->waitmsg->pid;
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
DEBUG()
{
	int i;
	Proc *p;

	print("DEBUG\n");
	for(i=0; i<conf.nproc; i++){
		p = procalloc.arena+i;
		if(p->state != Dead)
			print("%d:%s upc %lux %s ut %ld st %ld %lux\n",
				p->pid, p->text, p->pc, statename[p->state],
				p->time[0], p->time[1], p->qlock);
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

	/*
	 * Kernel stack
	 */
	p = newproc();
	p->upage = newpage(1, 0, USERADDR|(p->pid&0xFFFF));
	upa = p->upage->pa|KZERO;
	up = (User *)upa;

	/*
	 * Save time: only copy u-> data and useful stack
	 */
	memcpy((void*)upa, u, sizeof(User));
	n = USERADDR+BY2PG - (ulong)&lastvar;
	n = (n+32) & ~(BY2WD-1);	/* be safe & word align */
	memcpy((void*)(upa+BY2PG-n), (void*)((u->p->upage->pa|KZERO)+BY2PG-n), n);
	((User *)upa)->p = p;

	/*
	 * Refs
	 */
	incref(up->dot);
	for(n=0; n<=up->maxfd; n++)
		up->fd[n] = 0;
	up->maxfd = 0;

	/*
	 * Sched
	 */
	if(setlabel(&p->sched)){
		u->p = p;
		p->state = Running;
		p->mach = m;
		m->proc = p;
		spllo();
		strncpy(p->text, name, sizeof p->text);
		(*func)(arg);
		pexit(0, 1);
	}
	p->pgrp = u->p->pgrp;
	incref(p->pgrp);
	p->nchild = 0;
	p->parent = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	ready(p);
	flushmmu();
}
