typedef struct Conf	Conf;
typedef struct FFrame	FFrame;
typedef struct FPsave	FPsave;
typedef struct KMap	KMap;
typedef struct Lance	Lance;
typedef struct Lancemem	Lancemem;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct PMMU	PMMU;
typedef struct Mach	Mach;
typedef struct Ureg	Ureg;
typedef struct User	User;


#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

extern	Mach	mach0;
extern  void	(*kprofp)(ulong);

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	K_MAGIC

/*
 *  machine dependent definitions used by ../port/dat.h
 */

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
	long	fsr;
	long	fpreg[32];
	long	pad;		/* so fsr can be guaranteed at 4 mod 8 */
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
	ulong	nseg;		/* number of segments */
	ulong	nimage;		/* number of page cache image headers */
	ulong 	npagetab;	/* number of pte tables */
	ulong	nswap;		/* number of swap blocks */
	ulong	upages;		/* number of user pages */
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
	int	nfont;		/* GFont structs (devbit.c) */
	int	nsubfont;	/* Gsubfont structs (devbit.c) */
	int	nurp;		/* max urp conversations */
	int	nasync;		/* number of async protocol modules */
	int	npipe;		/* number of pipes */
	int	nservice;	/* number of services */
	int	nfsyschan;	/* number of filsys open channels */
	int	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	ipif;		/* Ip protocol interfaces */
	ulong	ip;		/* Ip conversations per interface */
	ulong	arp;		/* Arp table size */
	ulong	frag;		/* Ip fragment assemble queue size */
	int	cntrlp;		/* panic on ^P */
	int	dkif;		/* number of datakit interfaces */
};

/*
 *  mmu goo in the Proc structure
 */
struct PMMU
{
	int	pidonmach[MAXMACH];
	int	nmmuseg;	/* number of segments active in mmu */
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
	int	mmask;			/* 1<<m->machno */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Proc	*lproc;			/* last process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	fpstate;		/* state of fp registers on machine */
	char	pidhere[NTLBPID];	/* is this tlbpid possibly in this mmu? */
	int	lastpid;		/* last pid allocated on this machine */
	Proc	*pidproc[NTLBPID];	/* process that owns this pid on this mach */

	int	tlbfault;
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;

	int	stack[1];
};

#define	NERR	15
#define	NNOTE	5
struct User
{
	Proc	*p;
					/* &fpsave must be 4 mod 8 */
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
	ulong	svpsr;
	Note	note[NNOTE];
	short	nnote;
	short	notified;		/* sysnoted is due */
	Note	lastnote;
	int	(*notify)(void*, char*);
	void	*ureg;
	void	*dbgreg;		/* User registers for debugging in proc */
	ulong	svr7;
};

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

extern register Mach	*m;		/* R6 */
extern register User	*u;		/* R5 */

extern	uchar	*intrreg;

struct
{
	Lock;
	short	machs;
	short	exiting;
}active;
