typedef struct Alarm	Alarm;
typedef struct Bit3msg	Bit3msg;
typedef struct Blist	Blist;
typedef struct Block	Block;
typedef struct Chan	Chan;
typedef struct Conf	Conf;
typedef struct Dev	Dev;
typedef struct Dirtab	Dirtab;
typedef struct Env	Env;
typedef struct Envp	Envp;
typedef struct Envval	Envval;
typedef struct Error	Error;
typedef struct FPsave	FPsave;
typedef struct Label	Label;
typedef struct List	List;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct Mount	Mount;
typedef struct Mtab	Mtab;
typedef struct Noconv	Noconv;
typedef struct Nohdr	Nohdr;
typedef struct Noifc	Noifc;
typedef struct Note	Note;
typedef struct Nomsg	Nomsg;
typedef struct Nocall	Nocall;
typedef struct Orig	Orig;
typedef struct PTE	PTE;
typedef struct Page	Page;
typedef struct Pgrp	Pgrp;
typedef struct Proc	Proc;
typedef struct QLock	QLock;
typedef struct Qinfo	Qinfo;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct Seg	Seg;
typedef struct Stream	Stream;
typedef struct Ureg	Ureg;
typedef struct User	User;
typedef struct Syslog	Syslog;

typedef int Devgen(Chan*, Dirtab*, int, int, Dir*);

struct List
{
	void	*next;
};

struct Lock
{
	ulong	*sbsem;			/* addr of sync bus semaphore */
	ulong	pc;
};

struct Ref
{
	Lock;
	int	ref;
};

struct QLock
{
	Proc	*head;			/* next process waiting for object */
	Proc	*tail;			/* last process waiting for object */
	Lock	use;			/* to use object */
	Lock	queue;			/* to access list */
};

struct Label
{
	ulong	pc;
	ulong	sp;
};

struct Alarm
{
	List;
	Lock;
	int	busy;
	long	dt;		/* may underflow in clock(); must be signed */
	void	(*f)(void*);
	void	*arg;
};

struct Bit3msg
{
	ulong	cmd;
	ulong	addr;
	ulong	count;
	ulong	rcount;
};

#define	CHDIR	0x80000000L
#define	QPATH	0x0000FFFFL
struct Chan
{
	QLock;				/* general access */
	Ref;
	union{
		Chan	*next;		/* allocation */
		ulong	offset;		/* in file */
	};
	ushort	type;
	ushort	dev;
	ushort	mode;			/* read/write */
	ushort	flag;
	ulong	qid;
	Mount	*mnt;			/* mount point that derived Chan */
	ulong	mountid;
	int	fid;			/* for devmnt */
	Stream	*stream;		/* for stream channels */
	Chan	*mchan;			/* channel to mounted server */
	ulong	mqid;			/* qid of root of mount point */
};

struct	FPsave
{
	long	fpreg[32];
	long	fpstatus;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	npgrp;		/* process groups */
	ulong	npage0;		/* total physical pages of memory */
	ulong	npage;		/* total physical pages of memory */
	ulong	norig;		/* origins */
	ulong	npte;		/* contiguous page table entries */
	ulong	nmod;		/* single (modifying) page table entries */
	ulong	nalarm;		/* alarms */
	ulong	nchan;		/* channels */
	ulong	nenv;		/* distinct environment values */
	ulong	nenvchar;	/* environment text storage */
	ulong	npgenv;		/* environment files per process group */
	ulong	nmtab;		/* mounted-upon channels per process group */
	ulong	nmount;		/* mounts */
	ulong	nmntdev;	/* mounted devices (devmnt.c) */
	ulong	nmntbuf;	/* buffers for devmnt.c messages */
	ulong	nmnthdr;	/* headers for devmnt.c messages */
	ulong	nstream;	/* streams */
	ulong	nqueue;		/* stream queues */
	ulong	nblock;		/* stream blocks */
	ulong	nsrv;		/* public servers (devsrv.c) */
	ulong	nnoifc;		/* number of nonet interfaces */
	ulong	nnoconv;	/* number of nonet conversations/ifc */
	ulong	nurp;		/* max urp conversations */
};

struct Dev
{
	void	 (*reset)(void);
	void	 (*init)(void);
	Chan	*(*attach)(char*);
	Chan	*(*clone)(Chan*, Chan*);
	int	 (*walk)(Chan*, char*);
	void	 (*stat)(Chan*, char*);
	Chan	*(*open)(Chan*, int);
	void	 (*create)(Chan*, char*, int, ulong);
	void	 (*close)(Chan*);
	long	 (*read)(Chan*, void*, long);
	long	 (*write)(Chan*, void*, long);
	void	 (*remove)(Chan*);
	void	 (*wstat)(Chan*, char*);
	void	 (*errstr)(Error*, char*);
	void	 (*userstr)(Error*, char*);
};

struct Dirtab
{
	char	name[NAMELEN];
	long	qid;
	long	length;
	long	perm;
};

struct Env
{
	Lock;
	Envval	*val;
	char	name[NAMELEN];
	Env	*next;			/* in chain of Envs for a pgrp */
	int	pgref;			/* # pgrps pointing here */
};

struct Envp
{
	Env	*env;
	int	chref;			/* # chans from pgrp pointing here */
};

struct Mach
{
	int	machno;			/* physical id of processor */
	int	mmask;			/* 1<<m->machno */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	void	(*intr)(ulong, ulong);	/* pending interrupt */
	Proc	*intrp;			/* process that was interrupted */
	ulong	cause;			/* arg to intr */
	ulong	pc;			/* pc that was interrupted */
	char	pidhere[NTLBPID];	/* is this pid possibly in this mmu? */
	int	lastpid;		/* last pid allocated on this machine */
	Proc	*pidproc[NTLBPID];	/* process that owns this tlbpid on this mach */
	int	stack[1];
};

struct Mount
{
	Ref;				/* also used as a lock when playing lists */
	short	term;			/* terminates list */
	ulong	mountid;
	Mount	*next;
	Chan	*c;			/* channel replacing underlying channel */
};

struct Mtab
{
	Chan	*c;			/* channel mounted upon */
	Mount	*mnt;			/* what's mounted upon it */
};

enum{
	NUser,				/* note provided externally */
	NExit,				/* process should exit */
	NDebug,				/* process should hang */
};

struct Note
{
	char	msg[ERRLEN];
	int	flag;			/* whether system posted it */
};

struct Orig
{
	Lock;
	Orig	*next;			/* for allocation */
	ushort	nproc;			/* processes using it */
	ushort	npage;			/* sum of refs of pages in it */
	ushort	flag;
	ulong	va;			/* va of 0th pte */
	ulong	npte;			/* #pte's in list */
	PTE	*pte;
	Chan	*chan;			/* channel deriving segment (if open) */
	ushort	type;			/* of channel (which could be non-open) */
	ulong	qid;
	Chan	*mchan;
	ulong	mqid;
	ulong	minca;			/* base of region in chan */
	ulong	maxca;			/* end of region in chan */
int nmod;
};

struct Page
{
	Orig	*o;			/* origin of segment owning page */
	ulong	va;			/* virtual address */
	ulong	pa;			/* physical address */
	ushort	ref;
	Page	*next;
	Page	*prev;
};

struct Pgrp
{
	Ref;				/* also used as a lock when mounting */
	Pgrp	*next;
	ulong	pgrpid;
	char	user[NAMELEN];
	int	nmtab;			/* highest active mount table entry, +1 */
	int	nenv;			/* highest active env table entry, +1 */
	Mtab	*mtab;
	Envp	*etab;
};

struct PTE
{
	Proc	*proc;			/* process owning this PTE (0 in Orig) */
	PTE	*nextmod;		/* next at this va */
	PTE	*nextva;		/* next in this proc at higher va */
	Page	*page;
};

struct Rendez
{
	Lock;
	Proc	*p;
};

struct Seg
{
	Proc	*proc;			/* process owning this segment */
	Orig	*o;			/* root list of pte's */
	ulong	minva;			/* va of 0th pte (not necessarily Seg->o->va) */
	ulong	maxva;			/* va of last pte */
	PTE	*mod;			/* list of modified pte's */
	ulong	pad[3]; /**/
};

struct Proc
{
	Label	sched;
	Mach	*mach;			/* machine running this proc */
	char	text[NAMELEN];
	Proc	*rnext;			/* next process in run queue */
	Proc	*qnext;			/* next process on queue for a QLock */
	int	state;
	short	pidonmach[MAXMACH];	/* TLB pid on each mmu */
	Page	*upage;			/* BUG: should be unlinked from page list */
	Seg	seg[NSEG];
	ulong	pid;
	Lock	kidlock;		/* access to kid and sib */
	Proc	*pop;			/* some ascendant */
	Proc	*kid;			/* some descendant */
	Proc	*sib;			/* non-ascendant relatives (circular list) */
	int	nchild;
	QLock	wait;			/* exiting children to be waited for */
	ulong	waitmsg;
	Proc	*child;
	Proc	*parent;
	Pgrp	*pgrp;
	ulong	parentpid;
	ulong	time[6];		/* User, Sys, Real; child U, S, R */
	short	exiting;
	short	insyscall;
	int	fpstate;
	Lock	debug;			/* to access debugging elements of User */
	Rendez	*r;			/* rendezvous point slept on */
	Rendez	sleep;			/* place for tsleep and syssleep */
	int	wokeup;			/* whether sleep was interrupted */
	ulong	pc;			/* DEBUG only */

};

#define	NERR	15
#define	NFD	100
#define	NNOTE	5
struct User
{
	Proc	*p;
	Label	errlab[NERR];
	int	nerrlab;
	Error	error;
	FPsave	fpsave;			/* address of this is known by vdb */
	char	elem[NAMELEN];		/* last name element from namec */
	Chan	*slash;
	Chan	*dot;
	Chan	*fd[NFD];
	int	maxfd;			/* highest fd in use */
	/*
	 * I/O point for bit3 interface.  This is the easiest way to allocate
	 * them, but not the prettiest or most general.
	 */
	Bit3msg	kbit3;
	Bit3msg	ubit3;
	/*
	 * Rest of structure controlled by devproc.c and friends.
	 * lock(&p->debug) to modify.
	 */
	Note	note[NNOTE];
	short	nnote;
	short	notified;		/* sysnoted is due */
	int	(*notify)(void*, char*);
	void	*ureg;
};

/*
 * Fake kmap
 */
typedef void		KMap;
#define	VA(k)		((ulong)(k))
#define	kmap(p)		(KMap*)(p->pa|KZERO)
#define	kunmap(k)

/*
 *  operations available to a queue
 */
struct Qinfo
{
	void (*iput)(Queue*, Block*);	/* input routine */
	void (*oput)(Queue*, Block*);	/* output routine */
	void (*open)(Queue*, Stream*);
	void (*close)(Queue*);
	char *name;
};

/*
 *  We reference lance buffers via descriptors kept in host memory
 */
struct Block
{
	Block	*next;
	uchar	*rptr;		/* first unconsumed byte */
	uchar	*wptr;		/* first empty byte */
	uchar	*lim;		/* 1 past the end of the buffer */
	uchar	*base;		/* start of the buffer */
	uchar	flags;
	uchar	type;
};

/* flag bits */
#define S_DELIM 0x80
#define S_CLASS 0x07

/* type values */
#define M_DATA 0
#define M_CTL 1
#define M_HANGUP 2

/*
 *  a list of blocks
 */
struct Blist {
	Lock;
	Block	*first;		/* first data block */
	Block	*last;		/* last data block */
	long	len;		/* length of list in bytes */
	int	nb;		/* number of blocks in list */
};

/*
 *  a queue of blocks
 */
struct Queue {
	Blist;
	int	flag;
	Qinfo	*info;		/* line discipline definition */
	Queue	*other;		/* opposite direction, same line discipline */
	Queue	*next;		/* next queue in the stream */
	void	(*put)(Queue*, Block*);
	Rendez	r;
	void	*ptr;		/* private info for the queue */
};
#define QHUNGUP	0x1	/* flag bit meaning the stream has been hung up */
#define QINUSE	0x2
#define QHIWAT	0x4	/* queue has gone past the high water mark */	
#define QDEBUG	0x8

/*
 *  a stream head
 */
struct Stream {
	Lock;			/* structure lock */
	int	inuse;		/* number of processes in stream */
	int	opens;		/* number of processes with stream open */
	int	hread;		/* number of reads after hangup */
	int	type;		/* correclation with Chan */
	int	dev;		/* ... */
	int	id;		/* ... */
	QLock	rdlock;		/* read lock */
	QLock	wrlock;		/* write lock */
	Queue	*procq;		/* write queue at process end */
	Queue	*devq;		/* read queue at device end */
};
#define	RD(q)		((q)->other < (q) ? (q->other) : q)
#define	WR(q)		((q)->other > (q) ? (q->other) : q)
#define GLOBAL(a)	(((ulong)(a)) & 0x80000000)
#define STREAMTYPE(x)	((x)&0x1f)
#define STREAMID(x)	(((x)&~CHDIR)>>5)
#define STREAMQID(i,t)	(((i)<<5)|(t))
#define PUTNEXT(q,b)	(*(q)->next->put)((q)->next, b)
#define BLEN(b)		((b)->wptr - (b)->rptr)
#define QFULL(q)	((q)->flag & QHIWAT)
#define FLOWCTL(q)	{ if(QFULL(q->next)) flowctl(q); }

/*
 *  stream file qid's & high water mark
 */
enum {
	Shighqid = STREAMQID(1,0) - 1,
	Sdataqid = Shighqid,
	Sctlqid = Sdataqid-1,
	Slowqid = Sctlqid,
	Streamhi= (17*1024),	/* byte count high water mark */
	Streambhi= 16,		/* block count high water mark */
};

/*
 *  nonet constants
 */
enum {
	Nnomsg = 128,		/* max number of outstanding messages */
	Nnocalls = 5,		/* maximum queued incoming calls */
};

/*
 *  generic nonet header
 */
struct Nohdr {
	uchar	circuit[3];	/* circuit number */
	uchar	flag;
	uchar	mid;		/* message id */
	uchar	ack;		/* piggy back ack */
	uchar	remain[2];	/* count of remaing bytes of data */
	uchar	sum[2];		/* checksum (0 means none) */
};
#define NO_HDRSIZE 10
#define NO_NEWCALL	0x1	/* flag bit marking a new circuit */
#define NO_HANGUP	0x2	/* flag bit requesting hangup */
#define NO_ACKME	0x4	/* acknowledge this message */
#define NO_SERVICE	0x8	/* message includes a service name */

/*
 *  a buffer describing a nonet message
 */
struct Nomsg {
	QLock;
	Blist;
	int	mid;		/* sequence number */
	int	rem;		/* remaining */
	long	time;
	int	acked;
};

/*
 *  one exists for each Nonet conversation.
 */
struct Noconv {
	QLock;

	Stream	*s;
	Queue	*rq;		/* input queue */
	int	version;	/* incremented each time struct is changed */
	int	state;		/* true if listening */

	Nomsg	in[Nnomsg];	/* messages being received */
	int	rcvcircuit;	/* circuit number of incoming packets */

	uchar	ack[Nnomsg];	/* acknowledgements waiting to be sent */
	long	atime[Nnomsg];
	int	afirst;
	int	anext;

	QLock	xlock;		/* one trasmitter at a time */
	Rendez	r;		/* process waiting for an output mid */
	Nomsg	ctl;		/* for control messages */
	Nomsg	out[Nnomsg];	/* messages being sent */
	int	first;		/* first unacknowledged message */
	int	next;		/* next message buffer to use */
	int	lastacked;	/* last message acked */		
	Block	*media;		/* prototype media output header */
	Nohdr	*hdr;		/* nonet header inside of media header */

	Noifc	*ifc;
	int	kstarted;
	char	raddr[NAMELEN];	/* remote address */
	char	ruser[NAMELEN];	/* remote user */
	char	addr[NAMELEN];	/* local address */
	int	rexmit;		/* statistics */
	int	retry;
	int	bad;
	int	sent;
	int	rcvd;
};

/*
 *  an incoming call
 */
struct Nocall {
	Block	*msg;
	char	raddr[NAMELEN];
	long	circuit;
};

/*
 *  a nonet interface.  one exists for every stream that a 
 *  nonet multiplexor is pushed onto.
 */
struct Noifc {
	Lock;
	int	ref;
	char	name[NAMELEN];	/* interface name */		
	Queue	*wq;		/* interface output queue */
	Noconv	*conv;

	/*
	 *  media dependent
	 */
	int	maxtu;		/* maximum transfer unit */
	int	mintu;		/* minimum transfer unit */
	int	hsize;		/* media header size */
	void	(*connect)(Noconv *, char *);

	/*
	 *  calls and listeners
	 */
	QLock	listenl;
	Rendez	listenr;
	Lock	lock;
	Nocall	call[Nnocalls];
	int	rptr;
	int	wptr;
};

#define	PRINTSIZE	256
struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

extern register	Mach	*m;
extern register	User	*u;

/*
 * Process states
 */
enum
{
	Dead = 0,
	Moribund,
	Zombie,
	Ready,
	Scheding,
	Running,
	Queueing,
	MMUing,
	Exiting,
	Inwait,
	Wakeme,
	Broken,
};
extern	char	*statename[];

/*
 * Chan flags
 */
#define	COPEN	1	/* for i/o */
#define	CMOUNT	2	/* is result of a mount/bind */
#define	CCREATE	4	/* permits creation if CMOUNT */

/*
 * Proc.time
 */
enum
{
	TUser,
	TSys,
	TReal,
	TCUser,
	TCSys,
	TCReal,
};

/*
 * floating point registers
 */
enum
{
	FPinit,
	FPactive,
	FPinactive,
};

/*
 * Memory management
 */
#define	SSEG	0
#define	TSEG	1
#define	DSEG	2
#define	BSEG	3
#define	ESEG	4	/* used by exec to build new stack */

#define	OWRPERM	0x01	/* write permission */
#define	OPURE	0x02	/* original data mustn't be written */
#define	OCACHED	0x04	/* cached; don't discard on exit */

/*
 * Access types in namec
 */
enum
{
	Aaccess,	/* as in access, stat */
	Atodir,		/* as in chdir */
	Aopen,		/* for i/o */
	Amount,		/* to be mounted upon */
	Acreate,	/* file is to be created */
};

#define	NUMSIZE	12		/* size of formatted number */

#define	MACHP(n)	((Mach *)(MACHADDR+n*BY2PG))

extern	Conf	conf;
extern	ulong	initcode[];
extern	Dev	devtab[];
extern	char	devchar[];
extern	FPsave	initfp;

/*
 *  kernel based system log, passed between crashes
 */
#define SYSLOG		((Syslog *)(0xa0001B00))
#define SYSLOGMAGIC	0x87654321
struct Syslog
{
	ulong	magic;
	char	*next;
	char	buf[8*1024];
};
