typedef struct Alarm	Alarm;
typedef struct Alarms	Alarms;
typedef struct Block	Block;
typedef struct Blist	Blist;
typedef struct Chan	Chan;
typedef struct Dev	Dev;
typedef struct Dirtab	Dirtab;
typedef struct Egrp	Egrp;
typedef struct Env	Env;
typedef struct Envval	Envval;
typedef struct Etherpkt	Etherpkt;
typedef struct Fgrp	Fgrp;
typedef struct Ifile	Ifile;
typedef struct Image	Image;
typedef struct IOQ	IOQ;
typedef struct KIOQ	KIOQ;
typedef struct List	List;
typedef struct Mount	Mount;
typedef struct Mhead	Mhead;
typedef struct Network	Network;
typedef struct Note	Note;
typedef struct Page	Page;
typedef struct Palloc	Palloc;
typedef struct Pgrp	Pgrp;
typedef struct Proc	Proc;
typedef struct Pte	Pte;
typedef struct Qinfo	Qinfo;
typedef struct QLock	QLock;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct RWlock	RWlock;
typedef struct Segment	Segment;
typedef struct Stream	Stream;
typedef struct Waitq	Waitq;

typedef int Devgen(Chan*, Dirtab*, int, int, Dir*);

struct List
{
	void	*next;
};

struct Ref
{
	Lock;
	int	ref;
};

struct Rendez
{
	Lock;
	Proc	*p;
};

struct QLock
{
	Lock	use;			/* to access Qlock structure */
	Proc	*head;			/* next process waiting for object */
	Proc	*tail;			/* last process waiting for object */
	int	locked;			/* flag */
};

struct RWlock
{
	Lock;				/* Lock modify lock */
	QLock	x;			/* Mutual exclusion lock */
	QLock	k;			/* Lock for waiting writers held for readers */
	int	readers;		/* Count of readers in lock */
};

struct Alarm
{
	List;
	Lock;
	int	busy;
	long	dt;			/* may underflow in clock(); must be signed */
	void	(*f)(void*);
	void	*arg;
};

struct Alarms
{
	Lock;
	Proc	*head;
};

/* Block.flags */
#define S_DELIM 0x80
#define S_CLASS 0x07

/* Block.type */
#define M_DATA 0
#define M_CTL 1
#define M_HANGUP 2

struct Block
{
	Block	*next;
	Block	*list;			/* chain of block lists */
	uchar	*rptr;			/* first unconsumed byte */
	uchar	*wptr;			/* first empty byte */
	uchar	*lim;			/* 1 past the end of the buffer */
	uchar	*base;			/* start of the buffer */
	uchar	flags;
	uchar	type;
};

struct Blist {
	Lock;
	Block	*first;			/* first data block */
	Block	*last;			/* last data block */
	long	len;			/* length of list in bytes */
	int	nb;			/* number of blocks in list */
};

/*
 * Access types in namec
 */
enum
{
	Aaccess,			/* as in access, stat */
	Atodir,				/* as in chdir */
	Aopen,				/* for i/o */
	Amount,				/* to be mounted upon */
	Acreate,			/* file is to be created */
};

/*
 *  Chan.flags
 */
#define	COPEN	1			/* for i/o */
#define	CMSG	2			/* is the message channel for a mount */
#define	CCREATE	4			/* permits creation if c->mnt */
#define	CCEXEC	8			/* close on exec */
#define	CFREE	16			/* not in use */

struct Chan
{
	QLock	rdl;			/* read access */
	QLock	wrl;			/* write access */
	Lock	offl;			/* offset access */
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
	union {
		Stream	*stream;	/* for stream channels */
		void	*aux;
		Qid	pgrpid;		/* for #p/notepg */
		int	mntindex;	/* for devmnt */
	};
	Chan	*mchan;			/* channel to mounted server */
	Qid	mqid;			/* qid of root of mount point */
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
	long	 (*read)(Chan*, void*, long, ulong);
	long	 (*write)(Chan*, void*, long, ulong);
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
	Envval	*name;
	Envval	*val;
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
#define	ETHERMINTU	60		/* minimum transmit size */
#define	ETHERMAXTU	1514		/* maximum transmit size */
#define ETHERHDRSIZE	14		/* size of an ethernet header */

/*
 *  character based IO (mouse, keyboard, console screen)
 */
#define NQ	4096
struct IOQ
{
	Lock;
	uchar	buf[NQ];
	uchar	*in;
	uchar	*out;
	int	state;
	Rendez	r;
	union{
		void	(*puts)(IOQ*, void*, int);	/* output */
		int	(*putc)(IOQ*, int);		/* input */
	};
	union{
		int	(*gets)(IOQ*, void*, int);	/* input */
		int	(*getc)(IOQ*);			/* output */
	};
	void	*ptr;
};
struct KIOQ
{
	QLock;
	IOQ;
	int	repeat;
	int	c;
	int	count;
};
extern IOQ	lineq;
extern IOQ	printq;
extern IOQ	mouseq;
extern KIOQ	kbdq;

struct Mount
{
	ulong	mountid;
	Mount	*next;
	Mhead	*head;
	Chan	*to;			/* channel replacing underlying channel */
};

struct Mhead
{
	Chan	*from;			/* channel mounted upon */
	Mount	*mount;			/* what's mounted upon it */
	Mhead	*hash;			/* Hash chain */
};

enum{
	NUser,				/* note provided externally */
	NExit,				/* deliver note quietly */
	NDebug,				/* print debug message */
};

struct Note
{
	char	msg[ERRLEN];
	int	flag;			/* whether system posted it */
};

/* Fields for cache control of pages */
#define	PG_NOFLUSH	0
#define PG_TXTFLUSH	1
#define PG_DATFLUSH	2
/* Simulated modified and referenced bits */
#define PG_MOD		0x01
#define PG_REF		0x02

struct Page
{
	ulong	pa;			/* Physical address in memory */
	ulong	va;			/* Virtual address for user */
	ulong	daddr;			/* Disc address on swap */
	ushort	ref;			/* Reference count */
	char	lock;			/* Software lock */
	char	modref;			/* Simulated modify/reference bits */
	char	cachectl[MAXMACH];	/* Cache flushing control for putmmu */
	Image	*image;			/* Associated text or swap image */
	Page	*next;			/* Lru free list */
	Page	*prev;
	Page	*hash;			/* Image hash chains */
};

struct Swapalloc
{
	Lock;				/* Free map lock */
	int	free;			/* Number of currently free swap pages */
	char	*swmap;			/* Base of swap map in memory */
	char	*alloc;			/* Round robin allocator */
	char	*top;			/* Top of swap map */
	Rendez	r;			/* Pager kproc idle sleep */
}swapalloc;

struct Image
{
	Ref;
	Chan	*c;			/* Channel associated with running image */
	Qid 	qid;			/* Qid for page cache coherence checks */
	Qid	mqid;
	Chan	*mchan;
	ushort	type;			/* Device type of owning channel */
	Segment *s;			/* TEXT segment for image if running, may be null */
	Image	*hash;			/* Qid hash chains */
	Image	*next;			/* Free list */
};

struct Pte
{
	union {
		Pte	*next;			/* Free list */
		Page	*pages[PTEPERTAB];	/* Page map for this chunk of pte */
	};
};

/* Segment types */
#define SG_TYPE		007		/* Mask type of segment */
#define SG_TEXT		000
#define SG_DATA		001
#define SG_BSS		002
#define SG_STACK	003
#define SG_SHARED	004
#define SG_PHYSICAL	005
/* Segment flags */
#define SG_RONLY	040		/* Segment is read only */

#define HIGHWATER	((conf.npage*5)/100)
#define MAXHEADROOM	HIGHWATER*2	/* Silly but OK for debug */
#define PG_ONSWAP	1
#define pagedout(s)	(((ulong)s)==0 || (((ulong)s)&PG_ONSWAP))
#define swapaddr(s)	(((ulong)s)&~PG_ONSWAP)
#define onswap(s)	(((ulong)s)&PG_ONSWAP)

#define SEGMAXSIZE	(SEGMAPSIZE*PTEMAPMEM)

struct Segment
{
	Ref;
	QLock	lk;
	ushort	steal;			/* Page stealer lock */
	Segment	*next;			/* free list pointers */
	ushort	type;			/* segment type */
	ulong	base;			/* virtual base */
	ulong	top;			/* virtual top */
	ulong	size;			/* size in pages */
	ulong	fstart;			/* start address in file for demand load */
	ulong	flen;			/* length of segment in file */
	int	flushme;		/* maintain consistent icache for this segment */
	Image	*image;			/* image in file system attached to this segment */
	Page	*(*pgalloc)(ulong addr);/* SG_PHYSICAL page allocator */
	void	(*pgfree)(Page *);	/* SG_PHYSICAL page free */
	Pte	*map[SEGMAPSIZE];	/* segment pte map */
};

#define RENDHASH	32
#define REND(p,s)	((p)->rendhash[(s)%RENDHASH])
#define MNTHASH		32
#define MOUNTH(p,s)	((p)->mnthash[(s)->qid.path%MNTHASH])

struct Pgrp
{
	Ref;				/* also used as a lock when mounting */
	Pgrp	*next;			/* free list */
	int	index;			/* index in pgrp table */
	ulong	pgrpid;
	QLock	debug;			/* single access via devproc.c */
	RWlock	ns;			/* Namespace many read/one write lock */
	Mhead	*mnthash[MNTHASH];
	Proc	*rendhash[RENDHASH];	/* Rendezvous tag hash */
};

struct Egrp
{
	Ref;
	Egrp	*next;
	int	nenv;			/* highest active env table entry, +1 */
	QLock	ev;			/* for all of etab */
	Env	*etab;
};

#define	NFD	100
struct Fgrp
{
	Ref;
	Fgrp	*next;
	Chan	*fd[NFD];
	int	maxfd;			/* highest fd in use */
};

#define PGHSIZE	512
struct Palloc
{
	Lock;
	ulong	addr0;			/* next available ialloc addr in bank 0 */
	ulong	addr1;			/* next available ialloc addr in bank 1 */
	int	active;
	Page	*head;			/* most recently used */
	Page	*tail;			/* least recently used */
	ulong	freecount;		/* how many pages on free list now */
	ulong	user;			/* how many user pages */
	Page	*hash[PGHSIZE];
	Lock	hashlock;
	Rendez	r;			/* Sleep for free mem */
	QLock	pwait;			/* Queue of procs waiting for memory */
	int	wanted;			/* Do the wakeup at free */
};

struct Waitq
{
	Waitmsg	w;
	Waitq	*next;
};

enum					/* Argument to forkpgrp call */
{
	FPall 	  = 0,			/* Concession to back portablility */
	FPnote 	  = 1,
	FPnamespc = 2,
	FPenv	  = 4,
	FPclear	  = 8,
};

enum
{
	Forkpg	  = 1,
	Forkeg	  = 2,
	Forkfd	  = 4,
};

/*
 *  process memory segments - NSEG always last !
 */
enum
{
	SSEG, TSEG, DSEG, BSEG, ESEG, LSEG, SEG1, SEG2, NSEG
};

/*
 * Process states
 */
enum
{
	Dead = 0,
	Moribund,
	Ready,
	Scheding,
	Running,
	Queueing,
	Wakeme,
	Broken,
	Stopped,
	Rendezvous,
};

/*
 * devproc requests
 */
enum
{
	Proc_stopme = 1,
	Proc_exitme = 2,
};

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

struct Proc
{
	Label	sched;
	Mach	*mach;			/* machine running this proc */
	char	text[NAMELEN];
	char	user[NAMELEN];
	Proc	*rnext;			/* next process in run queue */
	Proc	*qnext;			/* next process on queue for a QLock */
	QLock	*qlock;			/* address of qlock being queued for DEBUG */
	ulong	qlockpc;		/* pc of last call to qlock */
	int	state;
	char	*psstate;		/* What /proc/???/status reports */
	Page	*upage;			/* BUG: should be unlinked from page list */
	Segment	*seg[NSEG];
	ulong	pid;

	Lock	exl;			/* Lock count and waitq */
	Waitq	*waitq;			/* Exited processes wait children */
	int	nchild;			/* Number of living children */
	int	nwait;			/* Number of uncollected wait records */
	Rendez	waitr;			/* Place to hang out in wait */
	Proc	*parent;

	Pgrp	*pgrp;			/* Process group for notes and namespace */
	Egrp 	*egrp;			/* Environment group */
	Fgrp	*fgrp;			/* File descriptor group */

	ulong	parentpid;
	ulong	time[6];		/* User, Sys, Real; child U, S, R */
	short	insyscall;
	int	fpstate;
	Lock	debug;			/* to access debugging elements of User */
	Rendez	*r;			/* rendezvous point slept on */
	Rendez	sleep;			/* place for tsleep and syssleep */
	int	notepending;		/* note issued but not acted on */
	ulong	pc;			/* DEBUG only */
	int	kp;			/* true if a kernel process */
	Proc	*palarm;		/* Next alarm time */
	ulong	alarm;			/* Time of call */
	int 	hasspin;		/* I hold a spin lock */
	int	newtlb;			/* Pager has touched my tables so I must flush */
	int	procctl;		/* Control for /proc debugging */

	ulong	rendtag;		/* Tag for rendezvous */ 
	ulong	rendval;		/* Value for rendezvous */
	Proc	*rendhash;		/* Hash list for tag values */
	/*
	 *  machine specific MMU goo
	 */
	PMMU;
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
 *  Queue.flag
 */
#define QHUNGUP	0x1			/* flag bit meaning the stream has been hung up */
#define QINUSE	0x2
#define QHIWAT	0x4			/* queue has gone past the high water mark */	
#define QDEBUG	0x8

struct Queue {
	Blist;
	int	flag;
	Qinfo	*info;			/* line discipline definition */
	Queue	*other;			/* opposite direction, same line discipline */
	Queue	*next;			/* next queue in the stream */
	void	(*put)(Queue*, Block*);
	QLock	rlock;			/* mutex for processes sleeping at r */
	Rendez	r;			/* standard place to wait for flow control */
	Rendez	*rp;			/* where flow control wakeups go to */
	void	*ptr;			/* private info for the queue */
};

struct Stream {
	QLock;				/* structure lock */
	short	inuse;			/* number of processes in stream */
	short	opens;			/* number of processes with stream open */
	ushort	hread;			/* number of reads after hangup */
	ushort	type;			/* correlation with Chan */
	ushort	dev;			/* ... */
	ushort	id;			/* ... */
	QLock	rdlock;			/* read lock */
	Queue	*procq;			/* write queue at process end */
	Queue	*devq;			/* read queue at device end */
	Block	*err;			/* error message from down stream */
	int	flushmsg;		/* flush up till the next delimiter */
};

/*
 *  useful stream macros
 */
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
	Streamhi= (9*1024),		/* byte count high water mark */
	Streambhi= 32,			/* block count high water mark */
};

/*
 *  a multiplexed network
 */
struct Ifile
{
	char	*name;
	void	(*fill)(Chan*, char*, int);
};
struct Network
{
	char	*name;
	int	nconv;			/* max # of conversations */
	Qinfo	*devp;			/* device end line disc */
	Qinfo	*protop;		/* protocol line disc */
	int	(*listen)(Chan*);
	int	(*clone)(Chan*);
	int	ninfo;
	Ifile	info[5];
};
#define MAJOR(q) ((q) >> 8)
#define MINOR(q) ((q) & 0xff)
#define DEVICE(a,i) (((a)<<8) | (i))

#define MAXSYSARG	6		/* for mount(fd, mpt, flag, arg, srv) */
#define	PRINTSIZE	256
#define	NUMSIZE		12		/* size of formatted number */

extern	FPsave	initfp;
extern	Conf	conf;
extern	ulong	initcode[];
extern	Dev	devtab[];
extern	char	devchar[];
extern	char	user[NAMELEN];
extern	char	*conffile;
extern	char	*errstrtab[];
extern	char	*statename[];
extern	Palloc 	palloc;
extern  Image	swapimage;

#define	CHDIR		0x80000000L
#define	CHAPPEND 	0x40000000L
#define	CHEXCL		0x20000000L
