#define Onemicrosecond			((Time)1000ULL)
#define Onemillisecond			((Time)1000*Onemicrosecond)
#define Onesecond				((Time)1000*Onemillisecond)

typedef vlong	Time;
typedef struct Schedevent Schedevent;

enum SEvent {
	SAdmit,		/* new proc arrives*/
	SRelease,		/* released, but not yet scheduled (on qreleased) */
	SRun,		/* one of this task's procs started running */
	SPreempt,		/* the running proc was preempted */
	SBlock,		/* none of the procs are runnable as a result of sleeping */
	SResume,		/* one or more procs became runnable */
	SDeadline,	/* proc's deadline */
	SYield,		/* proc reached voluntary early deadline */
	SSlice,		/* slice exhausted */
	SExpel,		/* proc is gone */
};
typedef enum SEvent	SEvent;

struct Schedevent {
	ulong	tid;		// Task ID
	Time		ts;		// Event time
	SEvent	etype;	// Event type
};

