typedef struct Conf	Conf;
typedef struct FPsave	FPsave;
typedef struct IMM	IMM;
typedef struct Irqctl	Irqctl;
typedef struct ISAConf	ISAConf;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct Notsave	Notsave;
typedef struct PMMU	PMMU;
typedef struct Map	Map;
typedef struct PCArch	PCArch;
typedef struct Proc	Proc;
typedef struct RMap RMap;
typedef struct Ureg	Ureg;

#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	(Q_MAGIC)


struct	Lock
{
	ulong	key;
	ulong	pc;
	ulong	sr;
	Proc	*p;
	ushort	isilock;
};

struct	Label
{
	ulong	sp;
	ulong	pc;
};

/*
 *  things saved in the Proc structure during a notify
 */
struct Notsave
{
	ulong	UNUSED;
};

/*
 *  MMU stuff in proc
 */
#define NCOLOR 1
struct PMMU
{
	ulong	UNUSED;
};

/*
 * FPsave.status
 */
enum
{
	FPinit,
	FPactive,
	FPinactive,
};

/*
 * This structure must agree with fpsave and fprestore asm routines
 */
struct	FPsave
{
	double	fpreg[32];
	union {
		double	fpscrd;
		struct {
			ulong	pad;
			ulong	fpscr;
		};
	};
	int	fpistate;	/* emulated fp */
	ulong	emreg[32][3];	/* emulated fp */
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
	int	nswppo;		/* max # of pageouts per segment pass */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	ialloc;		/* max interrupt time allocation in bytes */
	ulong	interps;	/* number of interpreter processes */
	ulong	pipeqsize;	/* size in bytes of pipe queues */
};

#include "../port/portdat.h"

/*
 *  machine dependent definitions not used by ../port/dat.h
 */

struct Mach
{
	/* OFFSETS OF THE FOLLOWING KNOWN BY l.s */
	int	machno;			/* physical id of processor (unused) */
	ulong	splpc;			/* pc of last caller to splhi (unused) */
	int	mmask;			/* 1<<m->machno (unused) */

	/* ordering from here on irrelevant */
	ulong	ticks;			/* of the clock since boot time */
	Proc	*proc;			/* current process on this processor */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void	*alarm;			/* alarms bound to this clock */
	int	nrdy;
	int	speed;	/* general system clock in MHz */
	long	oscclk;	/* oscillator frequency (MHz) */
	long	cpuhz;	/* general system clock (cycles) */
	long	clockgen;	/* clock generator frequency (cycles) */
	int	cputype;
	ulong	delayloop;
	ulong*	bcsr;
	IMM*	iomem;	/* MPC8xx internal i/o control memory */

	ulong	fairness;		/* for runproc */

	int	tlbfault;
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;
	vlong	fastclock;		/* last sampled value */
	int	flushmmu;		/* make current proc flush it's mmu state */

	/* MUST BE LAST */
	int	stack[1];
};
extern	Mach	mach0;

/*
 * Fake kmap
 */
typedef void		KMap;
#define	VA(k)		((ulong)(k))
#define	kmap(p)		(KMap*)((p)->pa)
#define	kunmap(k)

/*
 *  routines for things outside the PowerPC model
 */
struct PCArch
{
	char*	id;
	int	(*ident)(void);		/* this should be in the model */
	void	(*reset)(void);		/* this should be in the model */
	int	(*serialpower)(int);	/* 1 == on, 0 == off */
	int	(*modempower)(int);	/* 1 == on, 0 == off */

	void	(*intrinit)(void);
	int	(*intrenable)(int, int, Irqctl*);

	void	(*clockenable)(void);
};

/*
 *  a parsed .ini line
 */
#define ISAOPTLEN	16
#define NISAOPT		8

struct ISAConf {
	char	type[NAMELEN];
	ulong	port;
	ulong	irq;
	ulong	mem;
	int	dma;
	ulong	size;
	ulong	freq;
	uchar	bus;

	int	nopt;
	char	opt[NISAOPT][ISAOPTLEN];
};

struct Map {
	int	size;
	ulong	addr;
};

struct RMap {
	char*	name;
	Map*	map;
	Map*	mapend;

	Lock;
};

struct
{
	Lock;
	short	machs;
	short	exiting;
	short	ispanic;
}active;

extern register Mach	*m;
extern register Proc	*up;

