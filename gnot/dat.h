typedef struct Alarm	Alarm;
typedef struct Block	Block;
typedef struct Blist	Blist;
typedef struct Chan	Chan;
typedef struct Conf	Conf;
typedef struct Dev	Dev;
typedef struct Dirtab	Dirtab;
typedef struct Env	Env;
typedef struct Envp	Envp;
typedef struct Envval	Envval;
typedef struct FFrame	FFrame;
typedef struct FPsave	FPsave;
typedef struct KMap	KMap;
typedef struct Label	Label;
typedef struct List	List;
typedef struct Lock	Lock;
typedef struct MMU	MMU;
typedef struct MMUCache	MMUCache;
typedef struct Mach	Mach;
typedef struct Mount	Mount;
typedef struct Mtab	Mtab;
typedef struct Note	Note;
typedef struct Orig	Orig;
typedef struct PTE	PTE;
typedef struct Page	Page;
typedef struct Pgrp	Pgrp;
typedef struct Proc	Proc;
typedef struct Qinfo	Qinfo;
typedef struct QLock	QLock;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct Seg	Seg;
typedef struct Stream	Stream;
typedef struct Ureg	Ureg;
typedef struct User	User;

typedef int Devgen(Chan*, Dirtab*, int, int, Dir*);

struct List
{
	void	*next;
};

struct Lock
{
	char	key;			/* addr of sync bus semaphore */
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
	ulong	sp;
	ulong	pc;
	ushort	sr;
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

#define	CHDIR	0x80000000L
#define	CHAPPEND 0x40000000L
#define	CHEXCL	0x20000000L
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
	union {
		Stream	*stream;	/* for stream channels */
		void	*aux;
	};
	Chan	*mchan;			/* channel to mounted server */
	ulong	mqid;			/* qid of root of mount point */
};

struct	FPsave
{
	uchar	type;
	uchar	size;
	short	reserved;
	char	junk[212];	/* 68881: sizes 24, 180; 68882: 56, 212 */
	char	reg[3*4+8*12];
};

struct Conf
{
	int	nmach;		/* processors */
	int	nproc;		/* processes */
	int	npgrp;		/* process groups */
	ulong	npage0;		/* total physical pages of memory, bank 0 */
	ulong	npage1;		/* total physical pages of memory, bank 1 */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	npage;		/* total physical pages of memory */
	ulong	norig;		/* origins */
	ulong	npte;		/* contiguous page table entries */
	ulong	nmod;		/* single (modifying) page table entries */
	int	nalarm;		/* alarms */
	int	nchan;		/* channels */
	int	nenv;		/* distinct environment values */
	int	nenvchar;	/* environment text storage */
	int	npgenv;		/* environment files per process group */
	int	nmtab;		/* mounted-upon channels per process group */
	int	nmount;		/* mounts */
	int	nmntdev;	/* mounted devices (devmnt.c) */
	int	nmntbuf;	/* buffers for devmnt.c messages */
	int	nmnthdr;	/* headers for devmnt.c messages */
	int	nstream;	/* streams */
	int	nqueue;		/* stream queues */
	int	nblock;		/* stream blocks */
	int	nsrv;		/* public servers (devsrv.c) */
	int	nbitmap;	/* bitmap structs (devbit.c) */
	int	nbitbyte;	/* bytes of bitmap data (devbit.c) */
	int	nfont;		/* font structs (devbit.c) */
	int	nurp;		/* max urp conversations */
	int	nasync;		/* number of async protocol modules */
	int	npipe;		/* number of pipes */
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
	ulong	qid;
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

struct KMap
{
	KMap	*next;
	ulong	pa;
	ulong	va;
};
#define	VA(k)	((k)->va)

struct Mach
{
	int	machno;			/* physical id of processor */
	ulong	splpc;			/* pc of last caller to splhi */
	int	mmask;			/* 1<<m->machno */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Proc	*lproc;			/* last process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	fpstate;		/* state of fp registers on machine */
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
	QLock	*qlock;			/* address of qlock being queued for DEBUG */
	int	state;
	int	spin;			/* spinning instead of unscheduled */
	Page	*upage;			/* BUG: should be unlinked from page list */
	Seg	seg[NSEG];
	ulong	bssend;			/* initial top of bss seg */
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
	int	kp;			/* true if a kernel process */
};

struct MMU
{
	ulong	va;
	ulong	pa;
};

#define NMMU 16
struct MMUCache
{
	ulong	next;
	MMU	mmu[NMMU];
};

#define	NERR	15
#define	NNOTE	5
#define	NFD	100
struct User
{
	Proc	*p;
	int	nerrlab;
	Label	errlab[NERR];
	Error	error;
	FPsave	fpsave;			/* address of this is known by vdb */
	char	elem[NAMELEN];		/* last name element from namec */
	Chan	*slash;
	Chan	*dot;
	Chan	*fd[NFD];
	int	maxfd;			/* highest fd in use */
	/*
	 * Rest of structure controlled by devproc.c and friends.
	 * lock(&p->debug) to modify.
	 */
	Note	note[NNOTE];
	short	nnote;
	short	notified;		/* sysnoted is due */
	int	(*notify)(void*, char*);
	MMUCache mc;
	void	*ureg;
};

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
	void (*reset)(void);		/* initialization */
	Qinfo *next;
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
	QLock	rlock;		/* mutex for processes sleeping at r */
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
	short	inuse;		/* number of processes in stream */
	short	opens;		/* number of processes with stream open */
	ushort	hread;		/* number of reads after hangup */
	ushort	type;		/* correlation with Chan */
	ushort	dev;		/* ... */
	ushort	id;		/* ... */
	QLock	rdlock;		/* read lock */
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
	Streamhi= (9*1024),	/* byte count high water mark */
	Streambhi= 32,		/* block count high water mark */
};

#define	PRINTSIZE	256

extern Mach	*m;
extern User	*u;

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
#define	CCEXEC	8	/* close on exec */
#define	CFREE	16	/* not in use */

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
	FPdirty,
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

#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

extern	Conf	conf;
extern	ulong	initcode[];
extern	Dev	devtab[];
extern	char	devchar[];
extern	FPsave	initfp;
extern	Mach	mach0;

extern  void	(*kprofp)(ulong);
