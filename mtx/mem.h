/* none of this is meant to be correct yet */

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
#define	BY2PG		8192		/* bytes per page */
#define	WD2PG		(BY2PG/BY2WD)	/* words per page */
#define	PGSHIFT		13			/* log(BY2PG) */
#define 	ROUND(s, sz)	(((s)+(sz-1))&~(sz-1))
#define 	PGROUND(s)	ROUND(s, BY2PG)
#define BLOCKALIGN	8

#define	BY2PTE		8			/* bytes per pte entry */
#define	PTE2PG		(BY2PG/BY2PTE)	/* pte entries per page */

#define	MAXMACH		1			/* max # cpus system can run */
#define	KSTACK		4096			/* Size of kernel stack */

/*
 * Time
 */
#define	HZ		100			/* clock frequency */
#define	MS2HZ	(1000/HZ)
#define	TK2SEC(t)	((t)/HZ)		/* ticks to seconds */
#define	MS2TK(t)	(((t)*HZ+500)/1000)	/* milliseconds to closest tick */

/*
 * Magic registers
 */

#define	MACH	30		/* R30 is m-> */
#define	USER		29		/* R29 is up-> */


/*
 * Fundamental addresses
 */

#define	UREGSIZE	((8+32)*4)

/*
 * MMU
 *
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
#define PTEWRITE	0x9F0

/* TLB and MxEPN flag */
#define	TLBVALID	(1<<9)

#define	TLBSETS	32	/* number of tlb sets (603/603e) */

#define	PTEMAPMEM	(1024*1024)	
#define	PTEPERTAB	(PTEMAPMEM/BY2PG)
#define	SEGMAPSIZE	512
#define SSEGMAPSIZE	16

/*
 * Address spaces
 */

#define	UZERO	0			/* base of user address space */
#define	UTZERO	(UZERO+BY2PG)		/* first address in user text */
#define	USTKTOP	(TSTKTOP-TSTKSIZ*BY2PG)	/* byte just beyond user stack */
#define	TSTKTOP	KZERO	/* top of temporary stack */
#define	TSTKSIZ 100
#define	KZERO	0x80000000	/* base of kernel address space */
#define	KTZERO	(KZERO+0x400000)		/* first address in kernel text */
#define	USTKSIZE	(4*1024*1024)	/* size of user stack */

#define	PCI1			0x40000000
#define	PCI0			0xfd000000
#define	IOMEM		0xfe000000
#define	FALCON		0xfef80000
#define	RAVEN		0xfeff0000
#define	FLASHA		0xff000000
#define	FLASHB		0xff800000
#define	FLASHAorB	0xfff00000

#define isphys(x) (((ulong)x&KZERO)!=0)
