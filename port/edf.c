/* EDF scheduling */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/devrealtime.h"
#include	"../port/edf.h"

/* debugging */
int			edfprint = 0;
char			tabs[16] = "																";
int			ind;
#define DPRINT	if(edfprint)iprint
#define DENTER	ind++;if(edfprint)iprint
#define DLEAVE	ind--

char *edf_statename[] = {
	[EdfUnused] =		"Unused",
	[EdfExpelled] =		"Expelled",
	[EdfAdmitted] =	"Admitted",
	[EdfIdle] =			"Idle",
	[EdfAwaitrelease] =	"Awaitrelease",
	[EdfReleased] =		"Released",
	[EdfRunning] =		"Running",
	[EdfExtra] =		"Extra",
	[EdfPreempted] =	"Preempted",
	[EdfBlocked] =		"Blocked",
	[EdfDeadline] =		"Deadline",
};

static Cycintr	schedpoint;		/* First scheduling point */
static Ticks	utilization;		/* Current utilization */
static int		initialized;
static uvlong	fasthz;	
static Ticks	now;
QLock		edfschedlock;		/* schedulability, held for
							 */
Lock			edflock;

Task			tasks[Maxtasks];
int			ntasks;
Resource		resources[Maxresources];
int			nresources;
int			edf_stateupdate;

enum{
	Deadline,	/* Invariant for schedulability test: Deadline < Release */
	Release,
};

static int earlierrelease(Task *t1, Task *t2) {return t1->r < t2->r;}
static int earlierdeadline(Task *t1, Task *t2) {return t1->d < t2->d;}

/* Tasks waiting for release, head earliest release time */
Taskq		qwaitrelease =	{{0}, nil, earlierrelease};

/* Released tasks waiting to run, head earliest deadline */
Taskq		qreleased	=	{{0}, nil, earlierdeadline};

/* Exhausted EDF tasks, append at end */
Taskq		qextratime;

/* Tasks admitted waiting for first release */
Taskq		qadmit;

/* Running/Preempted EDF tasks, head running, one stack per processor */
Taskq		edfstack[MAXMACH];

void (*devrt)(Task*, Ticks, int);

static void		edf_intr(Ureg*, Cycintr*);
static void		edf_resched(Task *t);
static void		setΔ(void);
static void		testΔ(Task *thetask);
static char *	edf_testschedulability(Task *thetask);
static void		edf_setclock(void);

void
edf_init(void)
{
	if (initialized)
		return;
	ilock(&edflock);
	if (initialized){
		iunlock(&edflock);
		return;
	}
	fastticks(&fasthz);
	schedpoint.f = edf_intr;
	schedpoint.a = &schedpoint;
	schedpoint.when = 0;
	initialized = 1;
	iunlock(&edflock);
}

int
isedf(Proc *p)
{
	return p && p->task && p->task->state >= EdfIdle;
}

int
edf_anyready(void)
{
	/* If any edf tasks (with runnable procs in them) are released,
	 * at least one of them must be on the stack
	 */
	return edfstack[m->machno].head != nil;
}

static void
edfpush(Task *t)
{
	Taskq *q;

	DENTER("%.*s%d edfpush, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	q = edfstack + m->machno;
	assert(t->runq.n || (up && up->task == t));
	if (q->head){
		assert(q->head->state == EdfRunning);
		q->head->state = EdfPreempted;
		if(devrt) devrt(q->head, now, SPreempt);
	}
	t->rnext = q->head;
	if(devrt) devrt(t, now, SRun);
	q->head = t;
	DLEAVE;
}

static Task*
edfpop(void)
{
	Task *t;
	Taskq *q;

	DENTER("%.*s%d edfpop\n", ind, tabs, m->machno);
	q = edfstack + m->machno;
	if (t = q->head){
		assert(t->state == EdfRunning);
		q->head = t->rnext;
		t->rnext = nil;
		if (q->head){
			assert(q->head->state == EdfPreempted);
			q->head->state = EdfRunning;
			if(devrt) devrt(q->head, now, SRun);
		}
	}
	DLEAVE;
	return t;
}

static Task*
edfenqueue(Taskq *q, Task *t)
{
	Task *tt, **ttp;

	ilock(q);
	DENTER("%.*s%d edfenqueue, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	t->rnext = nil;
	if (q->head == nil) {
		q->head = t;
		DLEAVE;
		iunlock(q);
		return t;
	}
	SET(tt);
	for (ttp = &q->head; *ttp; ttp = &tt->rnext) {
		tt = *ttp;
		if (q->before && q->before(t, tt)) {
			t->rnext = tt;
			*ttp = t;
			break;
		}
	}
	if (*ttp == nil)
		tt->rnext = t;
	if (t != q->head)
		t = nil;
	DLEAVE;
	iunlock(q);
	return t;
}

static Task*
edfdequeue(Taskq *q)
{
	Task *t;

	DENTER("%.*s%d edfdequeue\n", ind, tabs, m->machno);
	ilock(q);
	if (t = q->head){
		q->head = t->rnext;
		t->rnext = nil;
	}
	iunlock(q);
	DLEAVE;
	return t;
}

static void
edfqremove(Taskq *q, Task *t)
{
	Task **tp;

	ilock(q);
	DENTER("%.*s%d edfqremove, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	for (tp = &q->head; *tp; tp = &(*tp)->rnext){
		if (*tp == t){
			*tp = t->rnext;
			DLEAVE;
			iunlock(q);
			return;
		}
	}
	DLEAVE;
	iunlock(q);
}

void
edf_block(Proc *p)
{
	Task *t, *pt;

	/* The current proc has blocked */
	ilock(&edflock);
	t = p->task;
	DENTER("%.*s%d edf_block, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);

	assert(t);
	assert(t->state == EdfRunning);
	if (t->runq.n){
		/* There's another runnable proc in the running task, leave task where it is */
		iunlock(&edflock);
		DLEAVE;
		return;
	}
	pt = edfpop();
	assert(pt == t);
	t->state = EdfBlocked;
	if(devrt) devrt(t, now, SBlock);
	DLEAVE;
	iunlock(&edflock);
}

static void
edfdeadline(Proc *p, SEvent why)
{
	Task *t, *nt;

	/* Task has reached its deadline, lock must be held */
	DENTER("%.*s%d edfdeadline, %s, %d\n", ind, tabs, m->machno, edf_statename[p->task->state], p->task->runq.n);
	SET(nt);
	if (p){
		nt = p->task;
		assert(nt);
		assert(nt->state == EdfRunning);
	}
	t = edfpop();

	if(p != nil && nt != t){
		DPRINT("%.*s%d edfdeadline, %s, %d\n", ind, tabs, m->machno, edf_statename[p->task->state], p->task->runq.n);
		iunlock(&edflock);
		assert(0 && p == nil || nt == t);
	}

	t->d = now;
	t->state = EdfDeadline;
	if(devrt) devrt(t, now, why);
	edf_resched(t);
	DLEAVE;
}

void
edf_deadline(Proc *p)
{
	DENTER("%.*s%d edf_deadline\n", ind, tabs, m->machno);
	/* Task has reached its deadline */
	ilock(&edflock);
	now = fastticks(nil);
	edfdeadline(p, SYield);
	iunlock(&edflock);
	DLEAVE;
}

char *
edf_admit(Task *t)
{
	char *err;

	if (t->state != EdfExpelled)
		return "task state";	/* should never happen */

	/* simple sanity checks */
	if (t->T == 0)
		return "T not set";
	if (t->C == 0)
		return "C not set";
	if (t->D > t->T)
		return "D > T";
	if (t->D == 0)	/* if D is not set, set it to T */
		t->D = t->T;
	if (t->C > t->D)
		return "C > D";

	qlock(&edfschedlock);
	if (err = edf_testschedulability(t)){
		qunlock(&edfschedlock);
		return err;
	}
	ilock(&edflock);
	DENTER("%.*s%d edf_admit, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	now = fastticks(nil);

	t->state = EdfAdmitted;
	if(devrt) devrt(t, t->d, SAdmit);
	if (up->task == t){
		DPRINT("%.*s%d edf_admitting self\n", ind, tabs, m->machno);
		/* Admitting self, fake reaching deadline */
		t->r = now;
		t->t = now + t->T;
		t->d = now + t->D;
		if(devrt) devrt(t, t->d, SDeadline);
		t->S = t->C;
		t->scheduled = now;
		t->state = EdfRunning;
		if(devrt) devrt(t, now, SRun);
		setΔ();
		assert(t->runq.n > 0 || (up && up->task == t));
		edfpush(t);
		edf_setclock();
	}else{
		if (t->runq.n){
			if (edfstack[m->machno].head == nil){
				t->state = EdfAdmitted;
				t->r = now;
				edf_release(t);
				setΔ();
				edf_resched(t);
			}else{
				edfenqueue(&qadmit, t);
			}
		}
	}
	DLEAVE;
	iunlock(&edflock);
	qunlock(&edfschedlock);
	return nil;
}

void
edf_expel(Task *t)
{
	Task *tt;

	qlock(&edfschedlock);
	ilock(&edflock);
	DENTER("%.*s%d edf_expel, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	now = fastticks(nil);
	switch(t->state){
	case EdfUnused:
	case EdfExpelled:
		/* That was easy */
		DLEAVE;
		iunlock(&edflock);
		qunlock(&edfschedlock);
		return;
	case EdfAdmitted:
	case EdfIdle:
		/* Just reset state */
		break;
	case EdfAwaitrelease:
		edfqremove(&qwaitrelease, t);
		break;
	case EdfReleased:
		edfqremove(&qreleased, t);
		break;
	case EdfRunning:
		/* Task must be expelling itself */
		tt = edfpop();
		assert(t == tt);
		break;
	case EdfExtra:
		edfqremove(&qextratime, t);
		break;
	case EdfPreempted:
		edfqremove(edfstack + m->machno, t);
		break;
	case EdfBlocked:
	case EdfDeadline:
		break;
	}
	t->state = EdfExpelled;
	if(devrt) devrt(t, now, SExpel);
	setΔ();
	DLEAVE;
	iunlock(&edflock);
	qunlock(&edfschedlock);
	return;
}

static void
edf_timer(void)
{
	Ticks used;
	Task *t;

	// If up is not set, we're running inside the scheduler
	// for non-real-time processes. 
	if (up && isedf(up)) {
		t = up->task;
		assert(t->scheduled > 0);
	
		used = now - t->scheduled;
		t->scheduled = now;

		if (t->r < now){
			if (t->S <= used)
				t->S = 0LL;
			else
				t->S -= used;
	
			if (t->d <= now || t->S == 0LL){
				/* Task has reached its deadline/slice, remove from queue */
				edfdeadline(up, SSlice);
				while (t = edfstack[m->machno].head){
					if (now < t->d)
						break;
					edfdeadline(nil, SSlice);
				}	
			}
		}
	}

	while((t = qwaitrelease.head) && t->r <= now){
		/* There's something waiting to be released and its time has come */
		edfdequeue(&qwaitrelease);
		edf_release(t);
	}
}

static void
edf_setclock(void)
{
	Ticks ticks;
	Task *t;

	DENTER("%.*s%d edf_setclock\n", ind, tabs, m->machno);
	ticks = ~0ULL;
	if ((t = qwaitrelease.head) && t->r < ticks)
		ticks = t->r;
	if (t = edfstack[m->machno].head){
		if (t->d < ticks)
			ticks = t->d;
		if (now + t->S < ticks)
			ticks = now + t->S;
	}
	if (schedpoint.when > now && schedpoint.when <= ticks){
		DLEAVE;
		return;
	}
	if (schedpoint.when){
		DPRINT("%.*s%d cycintrdel %T\n", ind, tabs, m->machno, ticks2time(schedpoint.when));
		cycintrdel(&schedpoint);
		schedpoint.when = 0;
	}
	if (ticks <= now){
		DPRINT("%.*s%d edf_timer: %T too late\n", ind, tabs, m->machno, ticks2time(now-ticks));
		ticks = now;
	}
	if (ticks != ~0ULL) {
		DPRINT("%.*s%d program timer in %T\n", ind, tabs, m->machno, ticks2time(ticks-now));
		schedpoint.when = ticks;
		cycintradd(&schedpoint);
		DPRINT("%.*s%d cycintradd %T\n", ind, tabs, m->machno, ticks2time(schedpoint.when-now));
	}
	clockintrsched();
	DLEAVE;
}
	
static void
edf_intr(Ureg *, Cycintr *cy)
{

	DENTER("%.*s%d edf_intr\n", ind, tabs, m->machno);
	/* Timer interrupt
	 * Timed events are:
	 * 1. release a task (look in qwaitrelease)
	 * 2. task reaches deadline
	 */
	now = fastticks(nil);

	assert(cy == &schedpoint && schedpoint.when <= now);

	if(active.exiting)
		return;

	ilock(&edflock);
	edf_timer();
	edf_setclock();
	iunlock(&edflock);
	DLEAVE;
	sched();
	splhi();
}

void
edf_bury(Proc *p)
{
	Task *t;
	Proc **pp;

	DPRINT("%.*s%d edf_bury\n", ind, tabs, m->machno);
	ilock(&edflock);
	now = fastticks(nil);
	if ((t = p->task) == nil){
		/* race condition? */
		iunlock(&edflock);
		DPRINT("%.*s%d edf bury race, pid %lud\n", ind, tabs, m->machno, p->pid);
		return;
	}
	assert(edfstack[m->machno].head == t);
	for (pp = t->procs; pp < t->procs + nelem(t->procs); pp++)
		if (*pp == p){
			t->nproc--;
			*pp = nil;
		}
	if (t->runq.head == nil){
		edfpop();
		t->state = EdfBlocked;
	}
	if (t->nproc == 0){
		assert(t->runq.head == nil);
		t->state = EdfIdle;
	}
	if(devrt) devrt(t, now, SBlock);
	p->task = nil;
	iunlock(&edflock);
}

void
edf_ready(Proc *p)
{
	Task *t;

	ilock(&edflock);
	DENTER("%.*s%d edf_ready, %s, %d\n", ind, tabs, m->machno, edf_statename[p->task->state], p->task->runq.n);
	if ((t = p->task) == nil){
		/* Must be a race */
		iunlock(&edflock);
		DPRINT("%.*s%d edf ready race, pid %lud\n", ind, tabs, m->machno, p->pid);
		return;
	}
	p->rnext = 0;
	p->readytime = m->ticks;
	p->state = Ready;
	t->runq.n++;
	if(t->runq.tail){
		t->runq.tail->rnext = p;
		t->runq.tail = p;
	}else{
		t->runq.head = p;
		t->runq.tail = p;

		/* first proc to become runnable in this task */
		now = fastticks(nil);
		edf_resched(t);
	}
	DLEAVE;
	iunlock(&edflock);
}

static void
edf_resched(Task *t)
{
	Task *xt;

	DENTER("%.*s%d edf_resched, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	if (t->nproc == 0){
		/* No member processes */
		if (t->state > EdfIdle){
			t->state = EdfIdle;
			if(devrt) devrt(t, now, SBlock);
		}
		DLEAVE;
		return;
	}
	if (t->runq.n == 0 && (up == nil || up->task != t)){
		/* Member processes but none runnable */
		DPRINT("%.*s%d edf_resched, nothing runnable\n", ind, tabs, m->machno);
		if (t->state == EdfRunning)
			edfpop();

		if (t->state >= EdfIdle && t->state != EdfBlocked){
			t->state = EdfBlocked;
			if(devrt) devrt(t, now, SBlock);
		}
		DLEAVE;
		return;
	}

	/* There are runnable processes */

	switch (t->state){
	case EdfUnused:
		iprint("%.*s%d attempt to schedule unused task\n", ind, tabs, m->machno);
	case EdfExpelled:
		DLEAVE;
		return;	/* Not admitted */
	case EdfIdle:
		/* task was idle, schedule release now or later */
		if (t->r < now){
			if (t->t < now)
				t->t = now + t->T;
			t->r = t->t;
		}
		edf_release(t);
		break;
	case EdfAwaitrelease:
	case EdfReleased:
	case EdfExtra:
	case EdfPreempted:
		/* dealt with by timer */
		break;
	case EdfAdmitted:
		/* test whether task can be started */
		if (edfstack[m->machno].head != nil){
			DLEAVE;
			return;
		}
		/* fall through */
	case EdfRunning:
		if (t->r <= now){
			if (t->t < now){
				DPRINT("%.*s%d edf_resched, rerelease\n", ind, tabs, m->machno);
				/* Period passed, rerelease */
				t->r = now;
				xt = edfpop();
				assert(xt == t);
				edf_release(t);
				DLEAVE;
				return;
			}
			if (now < t->d){
				if (t->S > 0){
					DPRINT("%.*s%d edf_resched, resume\n", ind, tabs, m->machno);
					/* Running, not yet at deadline, leave it */
					DLEAVE;
					return;
				}else
					t->d = now;
			}
			/* Released, but deadline is past, release at t->t */
			t->r = t->t;
		}
		DPRINT("%.*s%d edf_resched, schedule release\n", ind, tabs, m->machno);
		xt = edfpop();
		assert(xt == t);
		edfenqueue(&qwaitrelease, t);
		t->state = EdfAwaitrelease;
		edf_setclock();
		break;
	case EdfBlocked:
	case EdfDeadline:
		if (t->r <= now){
			if (t->t < now){
				DPRINT("%.*s%d edf_resched, rerelease\n", ind, tabs, m->machno);
				/* Period passed, rerelease */
				t->r = now;
				edf_release(t);
				DLEAVE;
				return;
			}
			if (now < t->d && (t->flags & Useblocking) == 0){
				if (t->S > 0){
					DPRINT("%.*s%d edf_resched, resume\n", ind, tabs, m->machno);
					/* Released, not yet at deadline, release (again) */
					t->state = EdfReleased;
					edfenqueue(&qreleased, t);
					if(devrt) devrt(t, now, SResume);
					DLEAVE;
					return;
				}else
					t->d = now;
			}
			/* Released, but deadline is past, release at t->t */
			t->r = t->t;
		}
		DPRINT("%.*s%d edf_resched, schedule release\n", ind, tabs, m->machno);
		edfenqueue(&qwaitrelease, t);
		t->state = EdfAwaitrelease;
		edf_setclock();
		break;
	}
	DLEAVE;
}

void
edf_release(Task *t)
{
	DENTER("%.*s%d edf_release, %s, %d\n", ind, tabs, m->machno, edf_statename[t->state], t->runq.n);
	assert(t->runq.n > 0 || (up && up->task == t));
	t->t = t->r + t->T;
	t->d = t->r + t->D;
	if(devrt) devrt(t, t->d, SDeadline);
	t->S = t->C;
	t->state = EdfReleased;
	edfenqueue(&qreleased, t);
	if(devrt) devrt(t, now, SRelease);
	edf_setclock();
	DLEAVE;
}

Proc *
edf_runproc(void)
{
	/* Return an edf proc to run or nil */
	
	Task *t, *nt;
	Proc *p;
//	Ticks when;
	static ulong nilcount;

	/* Figure out if the current proc should be preempted*/
	ilock(&edflock);
	assert(ind < nelem(tabs));
	now = fastticks(nil);

	/* first candidate is at the top of the stack of running procs */
	t = edfstack[m->machno].head;

	/* check out head of the release queue for a proc with a better deadline */
	nt = qreleased.head;

	if (t == nil && nt == nil){
		nilcount++;
		iunlock(&edflock);
		return nil;
	}
	DENTER("edf_runproc %lud\n", nilcount);
	if (nt && (t == nil || (nt->D < t->Δ && nt->d < t->d))){
		/* released task is better than current */
		DPRINT("%.*s%d edf_runproc: released\n", ind, tabs, m->machno);
		edfdequeue(&qreleased);
		assert(nt->runq.n >= 1);
		edfpush(nt);
		nt->state = EdfRunning;
		t = nt;
		t->scheduled = now;
	}else{
		DPRINT("%.*s%d edf_runproc: current\n", ind, tabs, m->machno);
	}

	assert (t->runq.n);

	/* Get first proc off t's run queue
	 * No need to lock runq, edflock always held to access runq
	 */
	t->state = EdfRunning;
	p = t->runq.head;
	if ((t->runq.head = p->rnext) == nil)
		t->runq.tail = nil;
	t->runq.n--;
	p->state = Scheding;
	if(p->mp != MACHP(m->machno))
		p->movetime = MACHP(0)->ticks + HZ/10;
	p->mp = MACHP(m->machno);
	edf_setclock();
	DLEAVE;
	iunlock(&edflock);
	return p;
}

static Lock	waitlock;

int
edf_waitlock(Lock *l)
{
	iprint("edf_waitlock\n");
	ilock(&waitlock);	/* can't afford normal locks here */
	if (l->key == 0){
		/* race on lock, don't block, just return */
		iunlock(&waitlock);
		return 0;
	}
	edf_block(up);
	up->rnext = l->edfwaiting;	/* enqueue on lock */
	l->edfwaiting = up;
	up->state = Scheding;
	up->lockwait = l;
	iunlock(&waitlock);
	return 1;
}

void
edf_releaselock(Lock *l)
{
	Proc *p;

	iprint("edf_releaselock\n");
	ilock(&waitlock);	/* can't afford normal locks here */
	if(l->edfwaiting == nil){
		iunlock(&waitlock);
		return;
	}
	p = l->edfwaiting;
	l->edfwaiting = p->rnext;
	assert(p->lockwait == l);
	if(p->state != Scheding)
		print("edf_releaselock: %s %lud %s\n", p->text, p->pid, statename[p->state]);
	p->lockwait = nil;
	iunlock(&waitlock);
	edf_ready(p);
}


/* Schedulability testing and its supporting routines */

static void
setΔ(void)
{
	Resource *r, **rr;
	Task **tt, *t;

	for (r = resources; r < resources + nelem(resources); r++){
		if (r->name == nil)
			continue;
		r->Δ = ~0LL;
		for (tt = r->tasks; tt < r->tasks + nelem(r->tasks); tt++)
			if (*tt && (*tt)->D < r->Δ)
				r->Δ = (*tt)->D;
	}
	for (t = tasks; t < tasks + nelem(tasks); t++){
		if (t->state < EdfIdle)
			continue;
		t->Δ = t->D;
		for (rr = t->res; rr < t->res + nelem(t->res); rr++)
			if (*rr && (*rr)->Δ < t->Δ)
				t->Δ = (*rr)->Δ;
	}
}

static void
testΔ(Task *thetask)
{
	Resource *r, **rr;
	Task **tt, *t;

	for (r = resources; r < resources + nelem(resources); r++){
		if (r->name == nil)
			continue;
		r->testΔ = ~0ULL;
		for (tt = r->tasks; tt < r->tasks + nelem(r->tasks); tt++)
			if (*tt && (*tt)->D < r->testΔ)
				r->testΔ = (*tt)->D;
	}
	for (t = tasks; t < tasks + nelem(tasks); t++){
		if (t->state <= EdfExpelled && t != thetask)
			continue;
		t->testΔ = t->D;
		for (rr = t->res; rr < t->res + nelem(t->res); rr++)
			if (*rr && (*rr)->testΔ < t->testΔ)
				t->testΔ = (*rr)->testΔ;
	}
}

static Ticks
blockcost(Ticks ticks, Task *thetask)
{
	Task *t;
	Ticks Cb;

	Cb = 0;
	for (t = tasks; t < tasks + Maxtasks; t++){
		if (t->state <= EdfExpelled && t != thetask)
			continue;
		if (t->testΔ <= ticks && ticks < t->D && Cb < t->C)
			Cb = t->C;
	}
	return Cb;
}

static Task *qschedulability;

static void
testenq(Task *t)
{
	Task *tt, **ttp;

	t->testnext = nil;
	if (qschedulability == nil) {
		qschedulability = t;
		return;
	}
	SET(tt);
	for (ttp = &qschedulability; *ttp; ttp = &tt->testnext) {
		tt = *ttp;
		if (t->testtime < tt->testtime
		|| (t->testtime == tt->testtime && t->testtype < tt->testtype)){
			t->testnext = tt;
			*ttp = t;
			return;
		}
	}
	assert(tt->testnext == nil);
	tt->testnext = t;
}

static char *
edf_testschedulability(Task *thetask)
{
	Task *t;
	Ticks H, G, Cb, ticks;
	int steps;

	/* initialize */
	testΔ(thetask);
	if (thetask && (thetask->flags & Verbose))
		pprint("schedulability test\n");
	qschedulability = nil;
	for (t = tasks; t < tasks + Maxtasks; t++){
		if (t->state <= EdfExpelled && t != thetask)
			continue;
		t->testtype = Release;
		t->testtime = 0;
		if (thetask && (thetask->flags & Verbose))
			pprint("\tInit: enqueue task %lud\n", t - tasks);
		testenq(t);
	}
	H=0;
	G=0;
	ticks = 0;
	for(steps = 0; steps < Maxsteps; steps++){
		t = qschedulability;
		qschedulability = t->testnext;
		ticks = t->testtime;
		switch (t->testtype){
		case Deadline:
			H += t->C;
			Cb = blockcost(ticks, thetask);
			if (thetask && (thetask->flags & Verbose))
				pprint("\tStep %3d, Ticks %T, task %lud, deadline, H += %T → %T, Cb = %T\n",
					steps, ticks2time(ticks), t - tasks,
					ticks2time(t->C), ticks2time(H), ticks2time(Cb));
			if (H+Cb>ticks)
				return "not schedulable";
			t->testtime += t->T - t->D;
			t->testtype = Release;
			testenq(t);
			break;
		case Release:
			if (thetask && (thetask->flags & Verbose))
				pprint("\tStep %3d, Ticks %T, task %lud, release, G  %T, C%T\n",
					steps, ticks2time(ticks), t - tasks,
					ticks2time(t->C), ticks2time(G));
			if(ticks && G <= ticks)
				return nil;
			G += t->C;
			t->testtime += t->D;
			t->testtype = Deadline;
			testenq(t);
			break;
		default:
			assert(0);
		}
	}
	return "probably not schedulable";
}

static uvlong
uvmuldiv(uvlong x, ulong num, ulong den)
{
	/* multiply, then divide, avoiding overflow */
	uvlong hi;

	hi = (x & 0xffffffff00000000LL) >> 32;
	x &= 0xffffffffLL;
	hi *= num;
	return (x*num + (hi%den << 32)) / den + (hi/den << 32);
}

Time
ticks2time(Ticks ticks)
{
	assert(ticks >= 0);
	return uvmuldiv(ticks, Onesecond, fasthz);
}

Ticks
time2ticks(Time time)
{
	assert(time >= 0);
	return uvmuldiv(time, fasthz, Onesecond);
}
