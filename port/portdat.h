typedef struct Alarms	Alarms;
typedef struct Block	Block;
typedef struct Chan	Chan;
typedef struct Crypt	Crypt;
typedef struct Dev	Dev;
typedef struct Dirtab	Dirtab;
typedef struct Egrp	Egrp;
typedef struct Evalue	Evalue;
typedef struct Fgrp	Fgrp;
typedef struct Image	Image;
typedef struct List	List;
typedef struct Mntcache Mntcache;
typedef struct Mount	Mount;
typedef struct Mntrpc	Mntrpc;
typedef struct Mntwalk	Mntwalk;
typedef struct Mnt	Mnt;
typedef struct Mhead	Mhead;
typedef struct Note	Note;
typedef struct Page	Page;
typedef struct Path	Path;
typedef struct Palloc	Palloc;
typedef struct Pgrps	Pgrps;
typedef struct Pgrp	Pgrp;
typedef struct Physseg	Physseg;
typedef struct Proc	Proc;
typedef struct Pte	Pte;
typedef struct Pthash	Pthash;
typedef struct QLock	QLock;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct RWlock	RWlock;
typedef struct Sargs	Sargs;
typedef struct Schedq	Schedq;
typedef struct Segment	Segment;
typedef struct Session	Session;
typedef struct Talarm	Talarm;
typedef struct Waitq	Waitq;
typedef int    Devgen(Chan*, Dirtab*, int, int, Dir*);

#include <auth.h>
#include <fcall.h>

struct Ref
{
	Lock;
	long	ref;
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
	QLock	k;			/* Lock for waiting writers */
	int	readers;		/* Count of readers in lock */
};

struct Talarm
{
	Lock;
	Proc	*list;
};

struct Alarms
{
	QLock;
	Proc	*head;
};

#define MAXSYSARG	5	/* for mount(fd, mpt, flag, arg, srv) */
struct Sargs
{
	ulong	args[MAXSYSARG];
};

/*
 * Access types in namec & channel flags
 */
enum
{
	Aaccess,			/* as in access, stat */
	Atodir,				/* as in chdir */
	Aopen,				/* for i/o */
	Amount,				/* to be mounted upon */
	Acreate,			/* file is to be created */

	COPEN	= 0x0001,		/* for i/o */
	CMSG	= 0x0002,		/* the message channel for a mount */
	CCREATE	= 0x0004,		/* permits creation if c->mnt */
	CCEXEC	= 0x0008,		/* close on exec */
	CFREE	= 0x0010,		/* not in use */
	CRCLOSE	= 0x0020,		/* remove on close */
	CRECOV	= 0x0040,		/* requires recovery */
	CCACHE	= 0x0080,		/* client cache */
};

struct Path
{
	Ref;
	Path	*hash;
	Path	*parent;
	Pthash	*pthash;
	char	elem[NAMELEN];
};

struct Chan
{
	Ref;
	Chan	*next;			/* allocation */
	Chan	*link;
	ulong	offset;			/* in file */
	ushort	type;
	ushort	dev;
	ushort	mode;			/* read/write */
	ushort	flag;
	Qid	qid;
	int	fid;			/* for devmnt */
	Path	*path;
	Mount	*mnt;			/* mount point that derived Chan */
	Mount	*xmnt;			/* Last mount point crossed */
	ulong	mountid;
	Mntcache *mcp;			/* Mount cache pointer */
	union {
		void	*aux;
		Qid	pgrpid;		/* for #p/notepg */
		Mnt	*mntptr;	/* for devmnt */
		ulong	mid;		/* for ns in devproc */
	};
	Chan	*mchan;			/* channel to mounted server */
	Qid	mqid;			/* qid of root of mount point */
	Session *session;
};

struct Dev
{
	void	(*reset)(void);
	void	(*init)(void);
	Chan*	(*attach)(char*);
	Chan*	(*clone)(Chan*, Chan*);
	int	(*walk)(Chan*, char*);
	void	(*stat)(Chan*, char*);
	Chan*	(*open)(Chan*, int);
	void	(*create)(Chan*, char*, int, ulong);
	void	(*close)(Chan*);
	long	(*read)(Chan*, void*, long, ulong);
	long	(*write)(Chan*, void*, long, ulong);
	void	(*remove)(Chan*);
	void	(*wstat)(Chan*, char*);
};

struct Dirtab
{
	char	name[NAMELEN];
	Qid	qid;
	long	length;
	long	perm;
};

enum
{
	NSMAX	=	1000,
	NSLOG	=	7,
	NSCACHE	=	(1<<NSLOG),
};

struct Pthash
{
	QLock;
	int	npt;
	Path	*root;
	Path	*hash[NSCACHE];
};

struct Mntwalk
{
	ulong	id;
	Mhead	*mh;
	Mount	*cm;
};

struct Mount
{
	ulong	mountid;
	Mount	*next;
	Mhead	*head;
	Chan	*to;			/* channel replacing channel */
	int	flag;
	char	spec[NAMELEN];
};

struct Mhead
{
	Chan	*from;			/* channel mounted upon */
	Mount	*mount;			/* what's mounted upon it */
	Mhead	*hash;			/* Hash chain */
};

struct Mnt
{
	Ref;			/* Count of attached channels */
	Chan	*c;		/* Channel to file service */
	Proc	*rip;		/* Reader in progress */
	Mntrpc	*queue;		/* Queue of pending requests on this channel */
	Mntrpc	*recwait;	/* List of rpc's with recovery pending */
	ulong	id;		/* Multiplexor id for channel check */
	Mnt	*list;		/* Free list */
	char	mux;		/* Set if the device does the multiplexing */
	int	flags;		/* recover/cache */
	char	recprog;	/* Recovery in progress */
	int	blocksize;	/* read/write block size */
	ushort	flushtag;	/* Tag to send flush on */
	ushort	flushbase;	/* Base tag of flush window for this buffer */
	Pthash	tree;		/* Path names from this mount point */
	char	*partial;	/* Outstanding read data */
	int	npart;		/* Sizeof remains */
};

enum
{
	NUser,				/* note provided externally */
	NExit,				/* deliver note quietly */
	NDebug,				/* print debug message */
};

struct Note
{
	char	msg[ERRLEN];
	int	flag;			/* whether system posted it */
};

enum
{
	PG_NOFLUSH	= 0,
	PG_TXTFLUSH	= 1,		/* flush icache */
	PG_DATFLUSH	= 2,		/* flush both i & d caches */
	PG_NEWCOL	= 3,		/* page has been recolored */

	PG_MOD		= 0x01,		/* software modified bit */
	PG_REF		= 0x02,		/* software referenced bit */
};

struct Page
{
	Lock;
	ulong	pa;			/* Physical address in memory */
	ulong	va;			/* Virtual address for user */
	ulong	daddr;			/* Disc address on swap */
	ushort	ref;			/* Reference count */
	char	modref;			/* Simulated modify/reference bits */
	char	color;			/* Cache coloring */
	char	cachectl[MAXMACH];	/* Cache flushing control for putmmu */
	Image	*image;			/* Associated text or swap image */
	Page	*next;			/* Lru free list */
	Page	*prev;
	Page	*hash;			/* Image hash chains */
};

struct Swapalloc
{
	Lock;				/* Free map lock */
	int	free;			/* currently free swap pages */
	uchar	*swmap;			/* Base of swap map in memory */
	uchar	*alloc;			/* Round robin allocator */
	uchar	*last;			/* Speed swap allocation */
	uchar	*top;			/* Top of swap map */
	Rendez	r;			/* Pager kproc idle sleep */
	ulong	highwater;		/* Pager start threshold */
	ulong	headroom;		/* Space pager frees under highwater */
}swapalloc;

struct Image
{
	Ref;
	Chan	*c;			/* channl to text file */
	Qid 	qid;			/* Qid for page cache coherence */
	Qid	mqid;
	Chan	*mchan;
	ushort	type;			/* Device type of owning channel */
	Segment *s;			/* TEXT segment for image if running */
	Image	*hash;			/* Qid hash chains */
	Image	*next;			/* Free list */
};

struct Pte
{
	Page	*pages[PTEPERTAB];	/* Page map for this chunk of pte */
	Page	**first;		/* First used entry */
	Page	**last;			/* Last used entry */
	Pte	*next;			/* Free list */
};

/* Segment types */
enum
{
	SG_TYPE		= 07,		/* Mask type of segment */
	SG_TEXT		= 00,
	SG_DATA		= 01,
	SG_BSS		= 02,
	SG_STACK	= 03,
	SG_SHARED	= 04,
	SG_PHYSICAL	= 05,
	SG_SHDATA	= 06,

	SG_RONLY	= 040,		/* Segment is read only */
};

#define PG_ONSWAP	1
#define onswap(s)	(((ulong)s)&PG_ONSWAP)
#define pagedout(s)	(((ulong)s)==0 || onswap(s))
#define swapaddr(s)	(((ulong)s)&~PG_ONSWAP)

#define SEGMAXSIZE	(SEGMAPSIZE*PTEMAPMEM)

struct Physseg
{
	ulong	attr;			/* Segment attributes */
	char	*name;			/* Attach name */
	ulong	pa;			/* Physical address */
	ulong	size;			/* Maximum segment size in pages */
	Page	*(*pgalloc)(Segment*, ulong);	/* Allocation if we need it */
	void	(*pgfree)(Page*);
};

struct Segment
{
	Ref;
	QLock	lk;
	ushort	steal;		/* Page stealer lock */
	Segment	*next;		/* free list pointers */
	ushort	type;		/* segment type */
	ulong	base;		/* virtual base */
	ulong	top;		/* virtual top */
	ulong	size;		/* size in pages */
	ulong	fstart;		/* start address in file for demand load */
	ulong	flen;		/* length of segment in file */
	int	flushme;	/* maintain icache for this segment */
	Image	*image;		/* text in file attached to this segment */
	Physseg *pseg;
	Pte	*map[SEGMAPSIZE];
};

enum
{
	RENDHASH =	32,		/* Hash to lookup rendezvous tags */
	MNTHASH	=	32,		/* Hash to walk mount table */
	NFD =		100,		/* per process file descriptors */
	PGHLOG  =	9,
	PGHSIZE	=	1<<PGHLOG,	/* Page hash for image lookup */
};
#define REND(p,s)	((p)->rendhash[(s)%RENDHASH])
#define MOUNTH(p,s)	((p)->mnthash[(s)->qid.path%MNTHASH])

struct Pgrp
{
	Ref;				/* also used as a lock when mounting */
	Pgrp	*next;			/* free list */
	ulong	pgrpid;
	QLock	debug;			/* single access via devproc.c */
	RWlock	ns;			/* Namespace n read/one write lock */
	QLock	nsh;
	Mhead	*mnthash[MNTHASH];
	Proc	*rendhash[RENDHASH];	/* Rendezvous tag hash */
};

struct Egrp
{
	Ref;
	QLock;
	Evalue	*entries;
	ulong	path;
};

struct Evalue
{
	char	*name;
	char	*value;
	int	len;
	ulong	path;
	Evalue	*link;
};

struct Fgrp
{
	Ref;
	Chan	*fd[NFD];
	int	maxfd;			/* highest fd in use */
};

struct Palloc
{
	Lock;
	ulong	p0, p1;			/* base of pages in bank 0/1 */
	ulong	np0, np1;		/* number of pages in bank 0/1 */
	Page	*head;			/* most recently used */
	Page	*tail;			/* least recently used */
	ulong	freecount;		/* how many pages on free list now */
	ulong	user;			/* how many user pages */
	Page	*hash[PGHSIZE];
	Lock	hashlock;
	Rendez	r;			/* Sleep for free mem */
	QLock	pwait;			/* Queue of procs waiting for memory */
	ulong	cmembase;		/* Key memory */
	ulong	cmemtop;
};

struct Waitq
{
	Waitmsg	w;
	Waitq	*next;
};

enum
{
	RFNAMEG		= (1<<0),
	RFENVG		= (1<<1),
	RFFDG		= (1<<2),
	RFNOTEG		= (1<<3),
	RFPROC		= (1<<4),
	RFMEM		= (1<<5),
	RFNOWAIT	= (1<<6),
	RFCNAMEG	= (1<<10),
	RFCENVG		= (1<<11),
	RFCFDG		= (1<<12)
};

/*
 *  process memory segments - NSEG always last !
 */
enum
{
	SSEG, TSEG, DSEG, BSEG, ESEG, LSEG, SEG1, SEG2, SEG3, SEG4, NSEG
};

enum
{
	Dead = 0,		/* Process states */
	Moribund,
	Ready,
	Scheding,
	Running,
	Queueing,
	Wakeme,
	Broken,
	Stopped,
	Rendezvous,

	Proc_stopme = 1, 	/* devproc requests */
	Proc_exitme,
	Proc_traceme,

	TUser = 0, 		/* Proc.time */
	TSys,
	TReal,
	TCUser,
	TCSys,
	TCReal,

	NERR = 15,
	NNOTE = 5,

	NSYSCALL = 37,		/* MUST be equal to the real thing!!!! */
	Pcounter = 6,		/* per process counters */
	 CSCNTR = 0,		/*  sched counter */
	 FAULTCNTR = 1,		/*  fault counter */
	 TLBCNTR = 2,		/*  utlbmiss */
};

struct Proc
{
	Label	sched;		/* known to l.s */
	char	*kstack;	/* known to l.s */
	Mach	*mach;		/* machine running this proc */
	char	text[NAMELEN];
	char	user[NAMELEN];
	Proc	*rnext;		/* next process in run queue */
	Proc	*qnext;		/* next process on queue for a QLock */
	QLock	*qlock;		/* addrof qlock being queued for DEBUG */
	int	state;
	char	*psstate;	/* What /proc/#/status reports */
	Page	*upage;		/* page from palloc */
	Segment	*seg[NSEG];
	ulong	pid;
	ulong	noteid;		/* Equivalent of note group */

	Lock	exl;		/* Lock count and waitq */
	Waitq	*waitq;		/* Exited processes wait children */
	int	nchild;		/* Number of living children */
	int	nwait;		/* Number of uncollected wait records */
	QLock	qwaitr;
	Rendez	waitr;		/* Place to hang out in wait */
	Proc	*parent;

	Pgrp	*pgrp;		/* Process group for namespace */
	Egrp 	*egrp;		/* Environment group */
	Fgrp	*fgrp;		/* File descriptor group */

	ulong	parentpid;
	ulong	time[6];	/* User, Sys, Real; child U, S, R */
	short	insyscall;
	int	fpstate;

	QLock	debug;		/* to access debugging elements of User */
	Proc	*pdbg;		/* the debugging process */
	ulong	procmode;	/* proc device file mode */
	int	hang;		/* hang at next exec for debug */
	int	procctl;	/* Control for /proc debugging */
	ulong	pc;		/* DEBUG only */

	Rendez	*r;		/* rendezvous point slept on */
	Rendez	sleep;		/* place for syssleep/debug */
	int	notepending;	/* note issued but not acted on */
	int	kp;		/* true if a kernel process */
	Proc	*palarm;	/* Next alarm time */
	ulong	alarm;		/* Time of call */
	int	newtlb;		/* Pager has changed my pte's, I must flush */

	ulong	rendtag;	/* Tag for rendezvous */ 
	ulong	rendval;	/* Value for rendezvous */
	Proc	*rendhash;	/* Hash list for tag values */

	ulong	twhen;
	Rendez	*trend;
	Proc	*tlink;
	int	(*tfn)(void*);
	void	(*kpfun)(void*);
	void	*kparg;

	FPsave	fpsave;		/* address of this is known by db */
	int	scallnr;	/* sys call number - known by db */
	Sargs	s;		/* address of this is known by db */
	int	nerrlab;
	Label	errlab[NERR];
	char	error[ERRLEN];
	char	elem[NAMELEN];	/* last name element from namec */
	Chan	*slash;
	Chan	*dot;

	Note	note[NNOTE];
	short	nnote;
	short	notified;	/* sysnoted is due */
	Note	lastnote;
	int	(*notify)(void*, char*);

	Mach	*mp;		/* machine this process last ran on */
	ulong	affinity;	/* affinity counter for that mach */

	void	*ureg;		/* User registers for notes */
	void	*dbgreg;	/* User registers for devproc */
	Notsave;

	/* statistics */
	ulong	counter[Pcounter];	/* specialized per process counters */
	ulong	syscall[NSYSCALL];

	/*
	 *  machine specific MMU
	 */
	PMMU;
};

struct Schedq
{
	Lock;
	Proc	*head;
	Proc	*tail;
	int	n;
};


enum
{
	PRINTSIZE =	256,
	MAXCRYPT = 	127,
	NUMSIZE	=	12,		/* size of formatted number */
	MB =		(1024*1024),
};

extern	Conf	conf;
extern	char*	conffile;
extern	int	cpuserver;
extern	int	cpuserver;
extern	Rune*	devchar;
extern	Dev	devtab[];
extern  char	eve[];
extern	char	hostdomain[];
extern	int	hwcurs;
extern	uchar	initcode[];
extern	FPsave	initfp;
extern  Queue	*kbdq;
extern  Queue	*mouseq;
extern  Ref	noteidalloc;
extern	int	nrdy;
extern	Palloc	palloc;
extern  Queue	*printq;
extern	char*	statename[];
extern  Image	swapimage;
extern	char	sysname[NAMELEN];
extern	Pthash	syspt;
extern	Talarm	talarm;

enum
{
	CHDIR =		0x80000000L,
	CHAPPEND = 	0x40000000L,
	CHEXCL =	0x20000000L,
	CHMOUNT	=	0x10000000L,
};

/*
 * auth messages
 */
enum
{
	FScchal	= 1,
	FSschal,
	FSok,
	FSctick,
	FSstick,
	FSerr,

	RXschal	= 0,
	RXstick	= 1,

	AUTHLEN	= 8,
};

/*
 *  mouse types
 */
enum
{
	Mouseother=	0,
	Mouseserial=	1,
	MousePS2=	2,
};
extern int mouseshifted;
extern int mousetype;
