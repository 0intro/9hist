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
typedef struct Etherpkt	Etherpkt;
typedef struct FPsave	FPsave;
typedef struct Hotmsg	Hotmsg;
typedef struct Lance	Lance;
typedef struct Lancemem	Lancemem;
typedef struct Label	Label;
typedef struct List	List;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct Mount	Mount;
typedef struct Mtab	Mtab;
typedef struct Note	Note;
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

struct Rendez
{
	Lock;
	Proc	*p;
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

struct Hotmsg
{
	ulong	cmd;
	ulong	param[5];
	Rendez	r;
	short	intr;			/* flag: interrupt has occurred */
	short	abort;			/* flag: don't interrupt */
};

#define	CHDIR	0x80000000L
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
	Qid	qid;
	Mount	*mnt;			/* mount point that derived Chan */
	ulong	mountid;
	int	fid;			/* for devmnt */
	union{
		Stream	*stream;	/* for stream channels */
		void	*aux;
		Qid	pgrpid;		/* for #p/notepg */
	};
	Chan	*mchan;			/* channel to mounted server */
	Qid	mqid;			/* qid of root of mount point */
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
	ulong	nasync;		/* number of async protocol modules */
	ulong	npipe;		/* number of pipes */
	ulong	maxialloc;	/* maximum bytes used by ialloc */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
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
};

struct Dirtab
{
	char	name[NAMELEN];
	Qid	qid;
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
	Qid	qid;
	Chan	*mchan;
	Qid	mqid;
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
	int	index;			/* index in pgrp table */
	ulong	pgrpid;
	char	user[NAMELEN];
	int	nmtab;			/* highest active mount table entry, +1 */
	int	nenv;			/* highest active env table entry, +1 */
	QLock	debug;			/* single access via devproc.c */
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
	ulong	bssend;			/* initial top of bss seg */
	ulong	pid;
	int	nchild;
	QLock	wait;			/* exiting children to be waited for */
	Waitmsg	waitmsg;		/* this is large but must be addressable */
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
	int	kp;			/* true if kernel process */

};

#define	NERR	20
#define	NFD	100
#define	NNOTE	5
struct User
{
	Proc	*p;
	Label	errlab[NERR];
	int	nerrlab;
	char	error[ERRLEN];
	FPsave	fpsave;			/* address of this is known by vdb */
	char	elem[NAMELEN];		/* last name element from namec */
	Chan	*slash;
	Chan	*dot;
	Chan	*fd[NFD];
	int	maxfd;			/* highest fd in use */
	/*
	 * I/O point for bit3 and hotrod interfaces.
	 * This is the easiest way to allocate
	 * them, but not the prettiest or most general.
	 */
	union{				/* for i/o from kernel */
		Bit3msg	kbit3;
		Hotmsg	khot;
	};
	union{				/* for i/o from user */
		Bit3msg	ubit3;
		Hotmsg	uhot;
	};
	union{				/* special location for Tflush */
		Bit3msg	fbit3;
		Hotmsg	fhot;
	};
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
#define PPN(x)		x

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
	void (*reset)(void);
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
	ulong	va;		/* virtual address if mapped for a dev */
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
	QLock	rlock;		/* mutex for r */
	Rendez	r;		/* standard place to wait for flow control */
	Rendez	*rp;		/* where flow control wakeups go to */
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
	QLock;			/* structure lock */
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
	Streamhi= (17*1024),	/* byte count high water mark */
	Streambhi= 32,		/* block count high water mark */
};

/*
 *  Ethernet packet buffers.
 */
struct Etherpkt {
	uchar d[6];
	uchar s[6];
	uchar type[2];
	uchar data[1500];
	uchar crc[4];
};
#define	ETHERMINMTU	60	/* minimum transmit size */

/*
 *  LANCE CSR3 (bus control bits)
 */
#define BSWP	0x4
#define ACON	0x2
#define BCON	0x1

/*
 *  system dependent lance stuff
 *  filled by lancesetup() 
 */
struct Lance
{
	ushort	lognrrb;	/* log2 number of receive ring buffers */
	ushort	logntrb;	/* log2 number of xmit ring buffers */
	ushort	nrrb;		/* number of receive ring buffers */
	ushort	ntrb;		/* number of xmit ring buffers */
	ushort	*rap;		/* lance address register */
	ushort	*rdp;		/* lance data register */
	ushort	busctl;		/* bus control bits */
	uchar	ea[6];		/* our ether addr */
	int	sep;		/* separation between shorts in lance ram
				    as seen by host */
	ushort	*lanceram;	/* start of lance ram as seen by host */
	Lancemem *lm;		/* start of lance ram as seen by lance */
	Etherpkt *rp;		/* receive buffers (host address) */
	Etherpkt *tp;		/* transmit buffers (host address) */
	Etherpkt *lrp;		/* receive buffers (lance address) */
	Etherpkt *ltp;		/* transmit buffers (lance address) */
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

extern	char	user[NAMELEN];
extern	char	*errstrtab[];

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
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	V_MAGIC
