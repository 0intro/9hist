typedef struct Conf	Conf;
typedef struct Lancepkt Lancepkt;
typedef struct FPsave	FPsave;
typedef struct Lance	Lance;
typedef struct Lancemem	Lancemem;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct MMU	MMU;
typedef struct Notsave	Notsave;
typedef struct PMMU	PMMU;
typedef struct Softtlb	Softtlb;
typedef struct Ureg	Ureg;
/*
 *  parameters for sysproc.c
 */
#define EXEC_MAGIC(magic)	(magic==V_MAGIC)

/*
 *  machine dependent definitions used by ../port/dat.h
 */

struct Lock
{
	int	val;
	ulong	pc;
	ulong	sr;
};

struct Label
{
	ulong	sp;
	ulong	pc;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	npage0;		/* total physical pages of memory */
	ulong	npage1;		/* total physical pages of memory */
	ulong	npage;		/* total physical pages of memory */
	ulong	upages;		/* user page pool */
	ulong	nimage;		/* number of page cache image headers */
	ulong	nswap;		/* number of swap pages */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	ipif;		/* Ip protocol interfaces */
	ulong	ip;		/* Ip conversations per interface */
	ulong	arp;		/* Arp table size */
	ulong	frag;		/* Ip fragment assemble queue size */
	ulong	debugger;	/* use processor 1 as a kernel debugger */
	ulong	ialloc;		/* bytes available for interrupt time allocation */
	ulong	pipeqsize;	/* size in bytes of pipe queues */
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

/*
 *  things saved in the Proc structure during a notify
 */
struct Notsave
{
	ulong	svstatus;
	ulong	svr1;
};

#include "../port/portdat.h"

/* First FOUR members offsets known by l.s */
struct Mach
{
	int	machno;			/* physical id of processor */
	Softtlb *stb;			/* Software tlb simulation  */
	Proc	*proc;			/* current process on this processor */
	ulong	splpc;			/* pc that called splhi() */
	int	tlbfault;		/* this offset known in l.s/utlbmiss() */

	/* Ok to change from here */
	ulong	ticks;			/* of the clock since boot time */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	lastpid;		/* last pid allocated on this machine */
	Proc	*pidproc[NTLBPID];	/* process that owns this tlbpid on this mach */

	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;
	int	ledval;			/* value last written to LED */
	int	otlbfault;
	int	nrdy;

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
#define AOUT_MAGIC	V_MAGIC || magic==M_MAGIC
/*
 *  LANCE CSR3 (bus control bits)
 */
#define BSWP	0x4
#define ACON	0x2
#define BCON	0x1

struct Lancepkt
{
	uchar	d[6];
	uchar	s[6];
	uchar	type[2];
	uchar	data[1500];
	uchar	crc[4];
};

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
	Lancepkt *rp;		/* receive buffers (host address) */
	Lancepkt *tp;		/* transmit buffers (host address) */
	Lancepkt *lrp;		/* receive buffers (lance address) */
	Lancepkt *ltp;		/* transmit buffers (lance address) */
};

struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

#define	MACHP(n)	((Mach *)(MACHADDR+n*BY2PG))

extern register Mach	*m;
extern register Proc	*up;
