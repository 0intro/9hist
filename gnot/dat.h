typedef struct Conf	Conf;
typedef struct FFrame	FFrame;
typedef struct FPsave	FPsave;
typedef struct KMap	KMap;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct MMU	MMU;
typedef struct MMUCache	MMUCache;
typedef struct Mach	Mach;
typedef struct PMMU	PMMU;
typedef struct Portpage	Portpage;
typedef struct Scsi	Scsi;
typedef struct Scsidata	Scsidata;
typedef struct Ureg	Ureg;
typedef struct User	User;

#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

extern	Mach	mach0;
extern  void	(*kprofp)(ulong);

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	A_MAGIC

/*
 *  machine dependent definitions used by ../port/dat.h
 */

struct Lock
{
	char	key;
	ulong	pc;
};

enum
{
	FPinit,
	FPactive,
	FPdirty,
};

struct	FPsave
{
	uchar	type;
	uchar	size;
	short	reserved;
	char	junk[212];	/* 68881: sizes 24, 180; 68882: 56, 212 */
	char	reg[3*4+8*12];
};

struct Label
{
	ulong	sp;
	ulong	pc;
	ushort	sr;
};

/*
 *  MMU info included in the Proc structure
 */
struct PMMU
{
	int	pmmu_dummy;
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
	int	nservice;	/* number of services */
	int	nfsyschan;	/* number of filsys open channels */
	ulong	maxialloc;	/* maximum bytes used by ialloc */
	int	copymode;	/* 0 is copy on write, 1 is copy on reference */
	int	portispaged;	/* ??? */
};

#include "../port/portdat.h"

/*
 *  machine dependent definitions not used by ../port/dat.h
 */
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
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Proc	*lproc;			/* last process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	fpstate;		/* state of fp registers on machine */

	int	tlbpurge;
	int	tlbfault;
	int	pfault;
	int	cs;
	int	syscall;
	int	spinlock;
	int	intr;

	int	stack[1];
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

/*
 *  gnot bus ports
 */
#define PORTSIZE	64
#define PORTSHIFT	6
#define PORTSELECT	PORT[32]

struct Portpage
{
	union {
		Lock;
		QLock;
	};
	int	 select;
};

extern Portpage portpage;
extern int	portispaged;
extern int	(*portservice[])(void);

/*
 *  for SCSI bus
 */
struct Scsidata
{
	uchar *	base;
	uchar *	lim;
	uchar *	ptr;
};

struct Scsi
{
	QLock;
	ulong	pid;
	ushort	target, lun;
	ushort	rflag;
	ushort	status;
	Scsidata cmd;
	Scsidata data;
	uchar *	save;
	uchar	cmdblk[16];
};

#define	NERR	15
#define	NNOTE	5
#define	NFD	100
struct User
{
	Proc	*p;
	int	nerrlab;
	Label	errlab[NERR];
	char	error[ERRLEN];
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
	void	*ureg;
	ushort	svsr;
	ushort	svvo;
	/*
	 *  mmu cache for preloading MMU.
	 *  should be replaced by software TLB? -- presotto
	 */
	MMUCache mc;
};

struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

extern Mach	*m;
extern User	*u;
