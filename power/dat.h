typedef struct Conf	Conf;
typedef struct Lancepkt Lancepkt;
typedef struct FPsave	FPsave;
typedef struct Cycmsg	Cycmsg;
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
#define AOUT_MAGIC	V_MAGIC

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
	/*
	 * I/O point for hotrod interfaces.
	 * This is the easiest way to allocate
	 * them, but not the prettiest or most general.
	 */
	Cycmsg	*kcyc;
	Cycmsg	*ucyc;
	Cycmsg	*fcyc;
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

struct Cycmsg
{
	ulong	cmd;
	ulong	param[5];
	Rendez	r;
	uchar	intr;			/* flag: interrupt has occurred */
};

/* First FOUR members offsets known by l.s */
struct Mach
{
	int	machno;			/* physical id of processor */
	Softtlb *stb;			/* Software tlb simulation  */
	Proc	*proc;			/* current process on this processor */
	ulong	splpc;			/* pc that called splhi() */
	/* Ok to change from here */
	ulong	ticks;			/* of the clock since boot time */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	char	pidhere[NTLBPID];	/* is this pid possibly in this mmu? */
	int	lastpid;		/* last pid allocated on this machine */
	Proc	*pidproc[NTLBPID];	/* tlb allocation table */
	Page	*ufreeme;		/* address of upage of exited process */
	Ureg	*ur;

	int	tlbfault;		/* this offset known in l.s/utlbmiss() */
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;
	int	ledval;			/* value last written to LED */
	int	otlbfault;
	Schedq	hiq;
	Schedq	loq;

	Callbk*	cbin;
	Callbk*	cbout;
	Callbk*	cbend;
	Callbk	calls[NCALLBACK];

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
