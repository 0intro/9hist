typedef struct Conf	Conf;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct MMU	MMU;
typedef struct Mach	Mach;
typedef struct PMMU	PMMU;
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
	char	key;
	ulong	pc;
};

struct Label
{
	ulong	sp;
	ulong	pc;
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
	int	nnoifc;
	int	nnoconv;
	ulong	ipif;		/* Ip protocol interfaces */
	ulong	ip;		/* Ip conversations per interface */
	ulong	arp;		/* Arp table size */
	ulong	frag;		/* Ip fragment assemble queue size */
	int	cntrlp;		/* panic on ^P */
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

struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

extern Mach	*m;
extern User	*u;
