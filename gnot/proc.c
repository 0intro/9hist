#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	<libg.h>
#include	<gnot.h>

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

int page_alloc(int);	/* !ORIG */
void page_free(int, int);

/*
 * Called as the last routine in main().  Wait for a process on the run queue, grab it,
 * and run it.  Note that in this routine the interrupts are enabled for the first time.
 */
void
schedinit(void)		/* never returns */
{
	Proc *p;

	/*
	 * At init time:  wait for a process on the run queue.
	 */
	for(;;){
		spllo();
		while (runq.head == 0)
			/* idle loop */;
		splhi();
		lock(&runq);
		if(runq.head != 0)
			break;
		unlock(&runq);
	}

	/*
	 * Set the u pointer and leave it there.  In fact, it might as well be a define.
	 */
	u = (User *) USERADDR;

	/*
	 * For later rescheduling.  Jumped to by sched() on stack switch.
	 */
	setlabel(&m->sched);

	/*
	 * Take a process from the run queue.  The run queue is locked here, and guaranteed
	 * to have a process on it.
	 */
	p = runq.head;
	if((runq.head = p->rnext) == 0)
		runq.tail = 0;
	unlock(&runq);

	/*
	 * Ok, here we go.  We have a process and we can start running.
	 */
	mapstack(p);
	gotolabel(&p->sched);
}

/*
 * Complete the restoring of a process after mapstack().  The interrupt level here is low.
 * However, since the process is not Running, it cannot be rescheduled at this point.  We
 * set the process state to Running.  If the previous process was dead, clean it up.
 */
void
restore(void)
{
	Proc *p = m->proc;	/* previous process */

	u->p->mach = m;
	m->proc = u->p;
	u->p->state = Running;
	if(p->state == Moribund){
		p->pid = 0;
		unlock(&p->debug);		/* set in pexit */
		p->upage->ref--;
		p->upage = 0;
		p->qnext = procalloc.free;
		procalloc.free = p;
		unlock(&procalloc);		/* set in pexit */
		p->state = Dead;
	}
}

/*
 * Save part of the process state.  Note: this is not the counterpart of restore().
 */
void
save(Balu *balu)
{
	fpsave(&u->fpsave);
	if(u->fpsave.type){
		if(u->fpsave.size > sizeof u->fpsave.junk)
			panic("fpsize %d max %d\n", u->fpsave.size, sizeof u->fpsave.junk);
		fpregsave(u->fpsave.reg);
		u->p->fpstate = FPactive;
		m->fpstate = FPdirty;
	}
	if(BALU->cr0 != 0xFFFFFFFF)	/* balu busy */
		memcpy(balu, BALU, sizeof(Balu));
	else{
		balu->cr0 = 0xFFFFFFFF;
		BALU->cr0 = 0xFFFFFFFF;
	}
}

/*
 * Reschedule the process.  We do not know whether the interrupt level is low or high
 * here, but we set it to low in any case.  If there is no other process to run, and
 * this process is Running, return immediately.  If this process is blocked, and there
 * is no other process to run, keep spinning until either this process or another
 * process becomes runnable.  If it was this process, we can return immediately.
 */
void
sched(void)
{
	Proc *p = u->p;
	long initfp;
	Balu balu;
	int saved = 0;

	/*
	 * Record that the process is spinning instead of blocked.  Ready() uses this
	 * information to decide what to do with the process.
	 */
	p->spin = 1;

	/*
	 * Look for a new process to be run.
	 */
	for (;;){
		spllo();

		/*
		 * Idle loop.  Return when this process becomes runnable.  If nothing else
		 * to do, start saving some of the process state.
		 */
		while (runq.head == 0)
			if(p->state == Running)
				return;
			else if(!saved){
				save(&balu);
				saved = 1;
			}
	
		/*
		 * Disable clock interrupts so that there will be no rescheduling in this
		 * section (on this machine).  If there is still a process on the run
		 * queue, break out of this loop.
		 */
		splhi();
		lock(&runq);
		if(runq.head != 0)
			break;
		unlock(&runq);
	}
	p->spin = 0;

	/*
	 * The first process on the run queue is the process we are going to run.  First
	 * save our state before we jump to schedinit.  If this process was running, put
	 * it on the run queue.
	 */
	if(p->state == Running){
		p->state = Ready;
		p->rnext = 0;
		runq.tail->rnext = p;
		runq.tail = p;
	}

	/*
	 * Save some process state (if we haven't done that already) and save/restore
	 * pc and sp.  We have to jump to schedinit() because we are going to remap the
	 * stack.
	 */
	if(!saved)
		save(&balu);
	if(setlabel(&p->sched) == 0)
		gotolabel(&m->sched);

	/*
	 * Interrupts are ok now.  Note that the process state is still not Running,
	 * so no rescheduling.
	 */
	spllo();

	/*
	 * Jumped to by schedinit.  Restore the process state.
	 */
	if(p->fpstate != m->fpstate)
		if(p->fpstate == FPinit){
			initfp = 0;
			fprestore((FPsave *) &initfp);
			m->fpstate = FPinit;
		}
		else{
			fpregrestore(u->fpsave.reg);
			fprestore(&u->fpsave);
			m->fpstate = FPdirty;
		}
	if(balu.cr0 != 0xFFFFFFFF)	/* balu busy */
		memcpy(BALU, &balu, sizeof balu);

	/*
	 * Complete restoring the process.
	 */
	restore();
}

void
ready(Proc *p)
{
	int s;

	s = splhi();
	if(p->spin){
		p->state = Running;
		splx(s);
		return;
	}
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
		p->kp = 0;
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
		print("double sleep %lux %d %d\n", r, r->p->pid, p->pid);
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
	k = kmap(p->upage);
	up = (User*)VA(k);
	if(flag!=NUser && (up->notify==0 || up->notified))
		up->nnote = 0;	/* force user's hand */
	else if(up->nnote == NNOTE-1){
		kunmap(k);
		return 0;
	}
	strcpy(up->note[up->nnote].msg, n);
	up->note[up->nnote++].flag = flag;
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
		*wp++ = TK2MS(*up++ + *ucp++);
		*wp++ = TK2MS(*up++ + *ucp  );
		*wp   = TK2MS(*up           );
		p->child = c;
		/*
		 * Pass info through back door, to avoid huge Proc's
		 */
		p->waitmsg = (((ulong)&w)&(BY2PG-1));
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

    done:

	lock(&procalloc);	/* sched() can't do this */
	lock(&c->debug);	/* sched() can't do this */
	c->state = Moribund;

	/*
	 * Call the scheduler.  This process gets cleaned up in restore() by the next
	 * process that runs.  That means that if there is no other process, we'll
	 * hang around for a little while.
	 */
	sched();	/* never returns */
}

ulong
pwait(Waitmsg *w)
{
	Proc *c, *p;
	KMap *k;
	ulong cpid;

	p = u->p;
again:
	while(canqlock(&p->wait)){
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
	k = kmap(c->upage);
	if(w)
		*w = *(Waitmsg*)(p->waitmsg|VA(k));
	cpid = ((Waitmsg*)(p->waitmsg|VA(k)))->pid;
	kunmap(k);
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

	print("DEBUG\n");
	for(i=0; i<conf.nproc; i++){
		p = procalloc.arena+i;
		if(p->state != Dead)
			print("%d:%s upc %lux %s ut %ld st %ld q %lux r %lux\n",
				p->pid, p->text, p->pc, statename[p->state],
				p->time[0], p->time[1], p->qlock, p->r);
	}
}

/*
 *  create a kernel process.  if func is nonzero put the process in the kernel
 *  process group, have it call func, and exit.
 *
 *  otherwise, the new process stays in the same process group and returns.
 */
Proc *
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	Chan *c;
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
		if(func)
			up->fd[n] = 0;
		else if(c = u->fd[n])		/* assign = */
			incref(c);
	if(!func)
		up->maxfd = 0;
	up->p = p;
	kunmap(k);

	/*
	 * Sched
	 */
	if(setlabel(&p->sched)){
		restore();
		if(func){
			(*func)(arg);
			pexit(0, 1);
		} else
			return 0;
	}
	if(func){
		if(kpgrp == 0){
			kpgrp = newpgrp();
			strcpy(kpgrp->user, "bootes");
		}
		p->pgrp = kpgrp;
	} else
		p->pgrp = u->p->pgrp;
	incref(p->pgrp);
	sprint(p->text, "%s.%.6s", name, u->p->pgrp->user);
	p->nchild = 0;
	p->parent = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	ready(p);
	flushmmu();
	return p;
}
