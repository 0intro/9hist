typedef struct Conf	Conf;
typedef struct FPsave	FPsave;
typedef struct ISAConf	ISAConf;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct MMU	MMU;
typedef struct Mach	Mach;
typedef struct Notsave	Notsave;
typedef struct PCArch	PCArch;
typedef struct PCIcfg	PCIcfg;
typedef struct PCMmap	PCMmap;
typedef struct Page	Page;
typedef struct PMMU	PMMU;
typedef struct Segdesc	Segdesc;
typedef struct Ureg	Ureg;

#define	MACHP(n)	(n==0? &mach0 : *(Mach**)0)

extern	Mach	mach0;
extern  void	(*kprofp)(ulong);

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	(I_MAGIC)

struct Lock
{
	ulong	key;
	ulong	sr;
};

struct Label
{
	ulong	sp;
	ulong	pc;
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

struct	FPsave
{
	ushort	control;
	ushort	r1;
	ushort	status;
	ushort	r2;
	ushort	tag;
	ushort	r3;
	ulong	pc;
	ushort	selector;
	ushort	r4;
	ulong	operand;
	ushort	oselector;
	ushort	r5;
	uchar	regs[80];	/* floating point registers */
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	monitor;	/* has monitor? */
	ulong	npage0;		/* total physical pages of memory */
	ulong	npage1;		/* total physical pages of memory */
	ulong	topofmem;	/* highest physical address + 1 */
	ulong	npage;		/* total physical pages of memory */
	ulong	upages;		/* user page pool */
	ulong	nimage;		/* number of page cache image headers */
	ulong	nswap;		/* number of swap pages */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	ulong	nfloppy;	/* number of floppy drives */
	ulong	nhard;		/* number of hard drives */
	ulong	ldepth;		/* screen depth */
	ulong	maxx;		/* screen width */
	ulong	maxy;		/* screen length */
	ulong	ialloc;		/* max interrupt time allocation in bytes */
	ulong	pipeqsize;	/* size in bytes of pipe queues */
};

/*
 *  MMU stuff in proc
 */
#define MAXMMU	4
#define MAXSMMU	1
#define NCOLOR 1
struct PMMU
{
	Page	*mmutop;	/* 1st level table */
	Page	*mmufree;	/* unused page table pages */
	Page	*mmuused;	/* used page table pages */
};

/*
 *  things saved in the Proc structure during a notify
 */
struct Notsave
{
	ulong	svflags;
	ulong	svcs;
	ulong	svss;
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
	int	nrdy;

	int	tlbfault;
	int	tlbpurge;
	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;

	int	stack[1];
};

/*
 * Fake kmap
 */
typedef void		KMap;
#define	VA(k)		((ulong)(k))
#define	kmap(p)		(KMap*)((p)->pa|KZERO)
#define	kunmap(k)

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

/*
 *  routines for things outside the PC model, like power management
 */
struct PCArch
{
	char	*id;
	void	(*reset)(void);		/* this should be in the model */
	int	(*cpuspeed)(int);	/* 0 = low, 1 = high */
	void	(*buzz)(int, int);	/* make a noise */
	void	(*lights)(int);		/* turn lights or icons on/off */
	int	(*serialpower)(int);	/* 1 == on, 0 == off */
	int	(*modempower)(int);	/* 1 == on, 0 == off */
	int	(*extvga)(int);		/* 1 == external, 0 == internal */
};

/*
 *  a parsed plan9.ini line
 */
struct ISAConf {
	char	type[NAMELEN];
	ulong	port;
	ulong	irq;
	int	dma;
	ulong	mem;
	ulong	size;
	ulong	freq;
	uchar	ea[6];
};

/*
 *  maps between ISA memory space and PCMCIA card memory space
 */
struct PCMmap
{
	ulong	ca;		/* card address */
	ulong	cea;		/* card end address */
	ulong	isa;		/* ISA address */
	int	len;		/* length of the ISA area */
	int	attr;		/* attribute memory */
	int	ref;
};


/*
 *  SCSI bus
 */
enum {
	MaxScsi		= 4,
	NTarget		= 8,
};
struct Target {
	int	ctlrno;
	int	target;
	uchar*	inq;
	uchar*	scratch;

	int	ok;
};
typedef int 	(*Scsiio)(Target*, int, uchar*, int, void*, int*);

#define MAXPCMCIA 8			/* maximum number of PCMCIA cards */
#define BOOTLINE ((char *)0x80000100)	/*  bootline passed by boot program */

extern PCArch	*arch;			/* PC architecture */
extern int	cpuflag;		/* true if this is a CPU */

extern Mach	*m;
extern Proc	*up;
extern char	filaddr[];
