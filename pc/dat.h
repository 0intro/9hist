typedef struct Conf	Conf;
typedef struct FPsave	FPsave;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct MMU	MMU;
typedef struct Mach	Mach;
typedef struct PMMU	PMMU;
typedef struct Segdesc	Segdesc;
typedef struct Ureg	Ureg;
typedef struct User	User;

#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

extern	Mach	mach0;
extern  void	(*kprofp)(ulong);

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	I_MAGIC

struct Lock
{
	ulong	key;
	ulong	pc;
};

struct Label
{
	ulong	sp;
	ulong	pc;
};

struct	FPsave
{
	int	type;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	npgrp;		/* process groups */
	ulong	npage0;		/* total physical pages of memory, bank 0 */
	ulong	npage1;		/* total physical pages of memory, bank 1 */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
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
	ulong	nbitmap;	/* bitmap structs (devbit.c) */
	ulong	nbitbyte;	/* bytes of bitmap data (devbit.c) */
	ulong	nfont;		/* font structs (devbit.c) */
	ulong	nurp;		/* max urp conversations */
	ulong	nasync;		/* number of async protocol modules */
	ulong	npipe;		/* number of pipes */
	ulong	nservice;	/* number of services */
	ulong	nfsyschan;	/* number of filsys open channels */
	ulong	maxialloc;	/* maximum bytes used by ialloc */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	portispaged;	/* ??? */
	ulong	nnoifc;
	ulong	nnoconv;
	ulong	cntrlp;		/* panic on ^P */
};

/*
 *  MMU stuff in proc
 */
struct PMMU
{
	MMU	*mmu;
};

#include "../port/portdat.h"

/*
 *  machine dependent definitions not used by ../port/dat.h
 */

struct Mach
{
	int	machno;			/* physical id of processor */
	ulong	splpc;			/* pc of last caller to splhi */
	int	mmask;			/* 1<<m->machno */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	fpstate;		/* state of fp registers on machine */

	int	tlbfault;
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	spinlock;
	int	intr;

	int	stack[1];
};

/*
 * Fake kmap
 */
typedef void		KMap;
#define	VA(k)		((ulong)(k))
#define	kmap(p)		(KMap*)(p->pa|KZERO)
#define	kunmap(k)

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
	ushort	svvo;
	ushort	svsr;
};

/*
 *  segment descriptor/gate
 */
struct Segdesc
{
	ulong	d0;
	ulong	d1;
};


struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

extern Mach	*m;
extern User	*u;
