typedef struct Bit3msg	Bit3msg;
typedef struct Conf	Conf;
typedef struct FPsave	FPsave;
typedef struct Hotmsg	Hotmsg;
typedef struct Lance	Lance;
typedef struct Lancemem	Lancemem;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct MMU	MMU;
typedef struct PMMU	PMMU;
typedef struct Softtlb	Softtlb;
typedef struct Ureg	Ureg;
typedef struct User	User;

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	V_MAGIC

/*
 *  machine dependent definitions used by ../port/dat.h
 */

struct Lock
{
	ulong	*sbsem;			/* addr of sync bus semaphore */
	ulong	pc;
};

struct Label
{
	ulong	pc;
	ulong	sp;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	npgrp;		/* process groups */
	ulong	npage0;		/* total physical pages of memory */
	ulong	npage1;		/* total physical pages of memory */
	ulong	npage;		/* total physical pages of memory */
	ulong	nseg;		/* number of segments */
	ulong	nimage;		/* number of page cache image headers */
	ulong 	npagetab;	/* number of pte tables */
	ulong	nswap;		/* number of swap pages */
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
	ulong	ipif;		/* Ip protocol interfaces */
	ulong	ip;		/* Ip conversations per interface */
	ulong	arp;		/* Arp table size */
	ulong	frag;		/* Ip fragment assemble queue size */
	ulong	cntrlp;		/* panic on ^P */
	ulong	dkif;		/* number of datakit interfaces */
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

struct	FPsave
{
	long	fpreg[32];
	long	fpstatus;
};

/*
 *  mmu goo in the Proc structure
 */
struct PMMU
{
	int	pidonmach[MAXMACH];
};

#include "../port/portdat.h"

/*
 *  machine dependent definitions not used by ../port/dat.h
 */
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
	uchar	intr;			/* flag: interrupt has occurred */
	uchar	abort;			/* flag: don't interrupt */
	ushort	wlen;			/* length of last message written */
};

struct Mach
{
	int	machno;			/* physical id of processor NB. MUST BE FIRST */
	Softtlb *stb;			/* Software tlb simulation NB. MUST BE SECOND */
	ulong	splpc;			/* pc that called splhi() */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	char	pidhere[NTLBPID];	/* is this pid possibly in this mmu? */
	int	lastpid;		/* last pid allocated on this machine */
	Proc	*pidproc[NTLBPID];	/* process that owns this tlbpid on this mach */
	Page	*ufreeme;		/* address of upage of exited process */

	int	tlbfault;		/* this offset known in l.s/utlbmiss() */
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	spinlock;
	int	intr;

	int	stack[1];
};

/*
 *  machine dependent definitions not used by ../port/dat.h
 */
struct Softtlb
{
	ulong	virt;
	ulong	phys;
};

/*
 * Fake kmap
 */
typedef void		KMap;
#define	VA(k)		((ulong)(k))
#define	kmap(p)		(KMap*)((p)->pa|KZERO)
#define	kunmap(k)
#define PPN(x)		x

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

struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

#define	MACHP(n)	((Mach *)(MACHADDR+n*BY2PG))

#define	NERR	15
#define	NNOTE	5
struct User
{
	Proc	*p;
	FPsave	fpsave;			/* address of this is known by db */
	int	nerrlab;
	Label	errlab[NERR];
	char	error[ERRLEN];
	char	elem[NAMELEN];		/* last name element from namec */
	Chan	*slash;
	Chan	*dot;
	/*
	 * Rest of structure controlled by devproc.c and friends.
	 * lock(&p->debug) to modify.
	 */
	Note	note[NNOTE];
	short	nnote;
	short	notified;		/* sysnoted is due */
	Note	lastnote;
	int	(*notify)(void*, char*);
	void	*ureg;			/* User registers for notes */
	void	*dbgreg;		/* User registers for debugging in proc */
	ulong	svstatus;
	/*
	 *  machine dependent User stuff
	 */
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
};

extern register Mach	*m;
extern register User	*u;
