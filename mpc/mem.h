/*
 * Memory and machine-specific definitions.  Used in C and assembler.
 */

/*
 * Sizes
 */

#define	BI2BY		8			/* bits per byte */
#define BI2WD		32			/* bits per word */
#define	BY2WD		4			/* bytes per word */
#define	BY2V		8			/* bytes per double word */
#define	BY2PG		4096			/* bytes per page */
#define	WD2PG		(BY2PG/BY2WD)		/* words per page */
#define	PGSHIFT		12			/* log(BY2PG) */
#define ROUND(s, sz)	(((s)+((sz)-1))&~((sz)-1))
#define PGROUND(s)	ROUND(s, BY2PG)
#define	CACHELINELOG	4
#define CACHELINESZ	(1<<CACHELINELOG)

#define	MAXMACH		1			/* max # cpus system can run */
#define	MACHSIZE	BY2PG
#define KSTACK		4096			/* Size of kernel stack */

/*
 * Time
 */
#define HZ		100			/* clock frequency */
#define	MS2HZ		(1000/HZ)		/* millisec per clock tick */
#define	TK2SEC(t)	((t)/HZ)		/* ticks to seconds */
#define	TK2MS(t)	((t)*MS2HZ)		/* ticks to milliseconds */
#define	MS2TK(t)	((t)/MS2HZ)		/* milliseconds to ticks */

/* Bit encodings for Machine State Register (MSR) */
#define MSR_POW		(1<<18)		/* Enable Power Management */
#define MSR_TGPR	(1<<17)		/* TLB Update registers in use */
#define MSR_ILE		(1<<16)		/* Interrupt Little-Endian enable */
#define MSR_EE		(1<<15)		/* External Interrupt enable */
#define MSR_PR		(1<<14)		/* Supervisor/User privelege */
#define MSR_FP		(1<<13)		/* Floating Point enable */
#define MSR_ME		(1<<12)		/* Machine Check enable */
#define MSR_SE		(1<<10)		/* Single Step */
#define MSR_BE		(1<<9)		/* Branch Trace */
#define MSR_IP		(1<<6)		/* Exception prefix 0x000/0xFFF */
#define MSR_IR		(1<<5)		/* Instruction MMU enable */
#define MSR_DR		(1<<4)		/* Data MMU enable */
#define MSR_RI		(1<<1)		/* Recoverable Exception */
#define MSR_LE		(1<<0)		/* Little-Endian enable */

/*
 * Traps
 */

/*
 * Magic registers
 */

#define	MACH		30		/* R30 is m-> */
#define	USER		29		/* R29 is up-> */

/*
 * Fundamental addresses
 */
#define	MACHADDR	((long)&mach0)

#define	UREGSIZE	((8+32)*4)

/*
 *  virtual MMU
 */
#define PTEMAPMEM	(1024*1024)	
#define	PTEPERTAB	(PTEMAPMEM/BY2PG)
#define SEGMAPSIZE	1984
#define SSEGMAPSIZE	16
#define PPN(x)		((x)&~(BY2PG-1))

/*
 * MMU
 */

/* L1 table entry and Mx_TWC flags */
#define PTEVALID	(1<<0)
#define PTEWT		(1<<1)	/* write through */
#define PTE4K		(0<<2)
#define PTE512K		(1<<2)
#define PTE8MB		(3<<2)
#define PTEG		(1<<4)	/* guarded */

/* L2 table entry and Mx_RPN flags (also PTEVALID) */
#define PTECI		(1<<1)	/*  cache inhibit */
#define PTESH		(1<<2)	/* page is shared; ASID ignored */
#define PTELPS		(1<<3)	/* large page size */
#define PTEWRITE	0x9F0

/*
 *  physical MMU - bogus at the momement
 */
#define	PTEUNCACHED	(1<<4)
#define	PTERONLY	(0<<1)
#define	PTEKERNEL	(0<<2)
#define	PTEUSER		(1<<2)
#define PTESIZE		(1<<7)

/* TLB and MxEPN flag */
#define	TLBVALID	(1<<9)

#define	TLBSETS	32	/* number of tlb sets */

/*
 *  Address spaces
 *
 *  User is at 0-2GB
 *  Kernel is at 2GB-4GB
 */
#define	UZERO		0			/* base of user address space */
#define	UTZERO		(UZERO+BY2PG)		/* first address in user text */
#define	KZERO		0x80000000		/* base of kernel address space */
#define	KTZERO		0xff000000		/* first address in kernel text */
#define	USTKTOP		(KZERO-BY2PG)		/* byte just beyond user stack */
#define	USTKSIZE	(16*1024*1024)		/* size of user stack */
#define	TSTKTOP		(USTKTOP-USTKSIZE)	/* end of new stack in sysexec */
#define TSTKSIZ 	100

/*
 * atlas board registers
 */
#define	INTMEM		0x80000000
#define NIMMEM		0x80100000
#define	FLASH0MEM	0x80200000
#define	FLASH1MEM	0x80400000
#define	SDRAMMEM	0x03000000
#define DRAMMEM		0xff000000		// to 0xffffffff: 16 Meg

#define	SIRAM	(INTMEM+0xC00)
#define	LCDCOLR	(INTMEM+0xE00)
#define	DPRAM	(INTMEM+0x2000)
#define	DPLEN1	0x200
#define	DPLEN2	0x400
#define	DPLEN3	0x800
#define	DPBASE	(DPRAM+DPLEN1)

#define	SCC1P	(INTMEM+0x3C00)
#define	I2CP	(INTMEM+0x3C80)
#define	MISCP	(INTMEM+0x3CB0)
#define	IDMA1P	(INTMEM+0x3CC0)
#define	SCC2P	(INTMEM+0x3D00)
#define	SPIP	(INTMEM+0x3D80)
#define	TIMERP	(INTMEM+0x3DB0)
#define	SMC1P	(INTMEM+0x3E80)
#define	DSP1P	(INTMEM+0x3EC0)
#define	SMC2P	(INTMEM+0x3F80)
#define	DSP2P	(INTMEM+0x3FC0)

#define KEEP_ALIVE_KEY 0x55ccaa33	/* clock and rtc register key */

#define getpgcolor(a)	0
