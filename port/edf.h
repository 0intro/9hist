enum {
	Nproc = 8,
	Nres = 8,
	Ntask = 8,
	Maxtasks = 20,
	Maxresources = 20,
	Maxsteps = Maxtasks * 2 * 100,	/* 100 periods of maximum # of tasks */

	/* Edf.flags field */
	Verbose = 0x1,
	Useblocking = 0x2,
};

typedef vlong	Time;
typedef uvlong	Ticks;

typedef struct Task		Task;
typedef struct Resource	Resource;
typedef struct Edf		Edf;
typedef struct Taskq		Taskq;

enum Edfstate {
	EdfUnused,		/* task structure not in use */
	EdfExpelled,		/* in initialization, not yet admitted */
	EdfAdmitted,		/* admitted, but not started */

	EdfIdle,			/* admitted, but no member processes */
	EdfAwaitrelease,	/* released, but too early (on qwaitrelease) */
	EdfReleased,		/* released, but not yet scheduled (on qreleased) */
	EdfRunning,		/* one of this task's procs is running (on stack) */
	EdfExtra,			/* one of this task's procs is running in extra time (off stack) */
	EdfPreempted,		/* the running proc was preempted */
	EdfBlocked,		/* none of the procs are runnable as a result of sleeping */
	EdfDeadline,		/* none of the procs are runnable as a result of scheduling */
};
typedef enum Edfstate	Edfstate;

struct Edf {
	/* time intervals */
	Ticks	D;		/* Deadline */
	Ticks	Delta;		/* Inherited deadline */
	Ticks	T;		/* period */
	Ticks	C;		/* Cost */
	Ticks	S;		/* Slice: time remaining in this period */
	/* times */
	Ticks	r;		/* (this) release time */
	Ticks	d;		/* (this) deadline */
	Ticks	t;		/* Start of next period, t += T at release */
	/* for schedulability testing */
	Ticks	testDelta;
	int		testtype;	/* Release or Deadline */
	Ticks	testtime;
	Task	*	testnext;
	/* other */
	Edfstate	state;
};

struct Task {
	QLock;
	Edf;
	Ticks	scheduled;
	Schedq	runq;		/* Queue of runnable member procs */
	Proc *	procs[Nproc];	/* List of member procs; may contain holes */
	int		nproc;		/* number of them */
	Resource*	res[Nres];		/* List of resources; may contain holes */
	int		nres;			/* number of them */
	char		*user;		/* mallocated */
	Dirtab	dir;
	int		flags;		/* e.g., Verbose */
	Task		*rnext;
};

struct Taskq
{
	Lock;
	Task*	head;
	int		(*before)(Task*, Task*);	/* ordering function for queue (nil: fifo) */
};

struct Resource
{
	char	*	name;
	Task *	tasks[Ntask];	/* may contain holes */
	int		ntasks;
	Ticks	Delta;
	/* for schedulability testing */
	Ticks	testDelta;
};

extern Lock		edftestlock;	/* for atomic admitting/expelling */
extern Task		tasks[Maxtasks];	/* may contain holes */
extern int			ntasks;
extern Resource	resources[Maxresources];	/* may contain holes */
extern int			nresources;
extern Lock		edflock;
extern Taskq		qwaitrelease;
extern Taskq		qreleased;
extern Taskq		qextratime;
extern Taskq		edfstack[];
extern int			edf_stateupdate;
extern void		(*devrt)(Task *, Ticks, int);
extern char *		edf_statename[];

#pragma	varargck	type	"T"		Time
#pragma	varargck	type	"U"		Ticks

/* Interface: */
void		edf_preempt(Task*);		/* Stop current task, administrate run time, leave on stack */
void		edf_deadline(Proc*);		/* Remove current task from edfstack, schedule its next release */
void		edf_release(Task*);		/* Release a task */
void		edf_schedule(Task *);	/* Run a released task: remove from qrelease, push onto edfstack */
char *	edf_admit(Task*);
void		edf_expel(Task*);
void		edf_bury(Proc*);		/* Proc dies, update Task membership */
void		edf_sched(Task*);		/* Figure out what to do with task (after a state change) */
int		isedf(Proc*);			/* Proc must be edf scheduled */
void		edf_stop(Proc*);		/* Just event generation; should probably be done differently */
int		edf_anyready(void);
void		edf_ready(Proc*);
Proc	*	edf_runproc(void);
void		edf_block(Proc*);		/* Edf proc has blocked, do the admin */
void		edf_init(void);
int		edf_waitlock(Lock*);
void		edf_releaselock(Lock*);
Time		ticks2time(Ticks);
Ticks	time2ticks(Time);
