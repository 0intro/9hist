/*
 * Memory and machine-specific definitions.  Used in C and assembler.
 */

/*
 * Sizes
 */

#define	BI2BY		8			/* bits per byte */
#define	BI2WD		32			/* bits per word */
#define	BY2WD		4			/* bytes per word */
#define 	BY2V		8			/* bytes per vlong */
#define	BY2PG		4096		/* bytes per page */
#define	WD2PG		(BY2PG/BY2WD)	/* words per page */
#define	PGSHIFT		12			/* log(BY2PG) */
#define 	ROUND(s, sz)	(((s)+(sz-1))&~(sz-1))
#define 	PGROUND(s)	ROUND(s, BY2PG)
#define	CACHELINELOG	4
#define	CACHELINESZ	(1<<CACHELINELOG)
#define	BLOCKALIGN	CACHELINESZ

#define	MHz	1000000

#define	BY2PTE		8				/* bytes per pte entry */
#define	PTE2PG		(BY2PG/BY2PTE)	/* pte entries per page */

#define	MAXMACH	1				/* max # cpus system can run */
#define	MACHSIZE	BY2PG
#define	KSTACK		4096			/* Size of kernel stack */

/*
 * Time
 */
#define	HZ		100			/* clock frequency */
#define	MS2HZ	(1000/HZ)
#define	TK2SEC(t)	((t)/HZ)		/* ticks to seconds */
#define	TK2MS(t)	((t)*MS2HZ)		/* ticks to milliseconds */
#define	MS2TK(t)	(((t)*HZ+500)/1000)	/* milliseconds to closest tick */

/*
 * Standard PPC Special Purpose Registers (OEA and VEA)
 */
#define DSISR	18
#define DAR	19		/* Data Address Register */
#define DEC	22		/* Decrementer */
#define SDR1	25
#define SRR0	26		/* Saved Registers (exception) */
#define SRR1	27
#define SPRG0	272		/* Supervisor Private Registers */
#define SPRG1	273
#define SPRG2	274
#define SPRG3	275
#define ASR	280		/* Address Space Register */
#define EAR	282		/* External Access Register (optional) */
#define TBRU	269		/* Time base Upper/Lower (Reading) */
#define TBRL	268
#define TBWU	284		/* Time base Upper/Lower (Writing) */
#define TBWL	285
#define PVR	287		/* Processor Version */
#define IABR	1010	/* Instruction Address Breakpoint Register (optional) */
#define DABR	1013	/* Data Address Breakpoint Register (optional) */
#define FPECR	1022	/* Floating-Point Exception Cause Register (optional) */
#define PIR	1023	/* Processor Identification Register (optional) */

#define IBATU(i)	(528+2*(i))	/* Instruction BAT register (upper) */
#define IBATL(i)	(529+2*(i))	/* Instruction BAT register (lower) */
#define DBATU(i)	(536+2*(i))	/* Data BAT register (upper) */
#define DBATL(i)	(537+2*(i))	/* Data BAT register (lower) */

/*
 * PPC604e-specific Special Purpose Registers (OEA)
 */
#define HID0		1008	/* Hardware Implementation Dependant Register 0 */
#define HID1		1009	/* Hardware Implementation Dependant Register 1 */
#define PMC1		953		/* Performance Monitor Counter 1 */
#define PMC2		954		/* Performance Monitor Counter 2 */
#define PMC3		957		/* Performance Monitor Counter 3 */
#define PMC4		958		/* Performance Monitor Counter 4 */
#define MMCR0	952		/* Monitor Control Register 0 */
#define MMCR1	956		/* Monitor Control Register 0 */
#define SIA		955		/* Sampled Instruction Address */
#define SDA		959		/* Sampled Data Address */

#define BIT(i)	(1<<(31-(i)))	/* Silly backwards register bit numbering scheme */

/*
 * Bit encodings for Machine State Register (MSR)
 */
#define MSR_POW		BIT(13)		/* Enable Power Management */
#define MSR_ILE		BIT(15)		/* Interrupt Little-Endian enable */
#define MSR_EE		BIT(16)		/* External Interrupt enable */
#define MSR_PR		BIT(17)		/* Supervisor/User privelege */
#define MSR_FP		BIT(18)		/* Floating Point enable */
#define MSR_ME		BIT(19)		/* Machine Check enable */
#define MSR_FE0		BIT(20)		/* Floating Exception mode 0 */
#define MSR_SE		BIT(21)		/* Single Step (optional) */
#define MSR_BE		BIT(22)		/* Branch Trace (optional) */
#define MSR_FE1		BIT(23)		/* Floating Exception mode 1 */
#define MSR_IP		BIT(25)		/* Exception prefix 0x000/0xFFF */
#define MSR_IR		BIT(26)		/* Instruction MMU enable */
#define MSR_DR		BIT(27)		/* Data MMU enable */
#define MSR_RI		BIT(30)		/* Recoverable Exception */
#define MSR_LE		BIT(31)		/* Little-Endian enable */

/*
 * Exception codes (trap vectors)
 */
#define CRESET	0x01
#define CMCHECK	0x02
#define CDSI		0x03
#define CISI		0x04
#define CEI		0x05
#define CALIGN	0x06
#define CPROG		0x07
#define CFPU		0x08
#define CDEC		0x09
#define CSYSCALL	0x0C
#define CTRACE	0x0D	/* optional */
#define CFPA		0x0E		/* optional */

/* PPC604e-specific: */
#define CPERF		0x0F		/* performance monitoring */
#define CIBREAK	0x13
#define CSMI		0x14

/*
 * Magic registers
 */

#define	MACH	30		/* R30 is m-> */
#define	USER		29		/* R29 is up-> */


/*
 *  virtual MMU
 */
#define PTEMAPMEM	(1024*1024)	
#define PTEPERTAB	(PTEMAPMEM/BY2PG)
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
#define PTE512K	(1<<2)
#define PTE8MB	(3<<2)
#define PTEG		(1<<4)	/* guarded */

/* L2 table entry and Mx_RPN flags (also PTEVALID) */
#define PTECI		(1<<1)	/*  cache inhibit */
#define PTESH		(1<<2)	/* page is shared; ASID ignored */
#define PTELPS		(1<<3)	/* large page size */
//#define PTEWRITE	0x9F0

#define	PTEKERNEL	(0<<2)
#define	PTEUSER		(1<<2)
#define PTESIZE		(1<<7)

#define	NTLBPID		16
#define	TLBPID(n)	((n)&(NTLBPID-1))

/*
 *  portable MMU bits for fault.c - though still machine specific
 */
//#define PTEVALID	(MMUPP|MMUV)
#define PTEWRITE	(2<<10)
#define	PTERONLY	(3<<10)
#define	PTEUNCACHED	(1<<4)

/*
 * physical MMU bits
 */

/* TLB and MxEPN flag */
#define	TLBVALID	(1<<9)

#define	TLBSETS	32	/* number of tlb sets (603/603e) */

/*
 * Address spaces
 */

#define	UZERO	0			/* base of user address space */
#define	UTZERO	(UZERO+BY2PG)		/* first address in user text */
#define	USTKTOP	(TSTKTOP-TSTKSIZ*BY2PG)	/* byte just beyond user stack */
#define	TSTKTOP	KZERO	/* top of temporary stack */
#define	TSTKSIZ 100
#define	KZERO	0x80000000		/* base of kernel address space */
#define	KTZERO	(KZERO+0x4000)	/* first address in kernel text */
#define	USTKSIZE	(4*1024*1024)		/* size of user stack */
#define	UREGSIZE	((8+32)*4)

#define	PCIMEM0		0xf0000000
#define	PCISIZE0		0x0e000000
#define	PCIMEM1		0xc0000000
#define	PCISIZE1		0x30000000
#define	IOMEM		0xfe000000
#define	IOSIZE		0x00800000
#define	FALCON		0xfef80000
#define	RAVEN		0xfeff0000
#define	FLASHA		0xff000000
#define	FLASHB		0xff800000
#define	FLASHAorB	0xfff00000

#define isphys(x) (((ulong)x&KZERO)!=0)

#define getpgcolor(a)	0
