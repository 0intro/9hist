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
#define ROUND(s, sz)	(((s)+(sz-1))&~(sz-1))
#define PGROUND(s)	ROUND(s, BY2PG)
#define	BLOCKALIGN	8

#define	MAXMACH		1			/* max # cpus system can run */

/*
 * Time
 */
#define	HZ		(100)				/* clock frequency */
#define	MS2HZ		(1000/HZ)			/* millisec per clock tick */
#define	TK2SEC(t)	((t)/HZ)			/* ticks to seconds */
#define	TK2MS(t)	((((ulong)(t))*1000)/HZ)	/* ticks to milliseconds */
#define	MS2TK(t)	((((ulong)(t))*HZ)/1000)	/* milliseconds to ticks */

/*
 * Fundamental addresses
 */
#define MACHADDR	(KZERO+0x00001000)

/*
 *  Address spaces:
 *
 *  We've direct mapped the DRAM's and all the
 *  special use space.  Since the DRAM's lie at
 *  0xc0000000 and since the Compaq boot loader
 *  copies the kernel to the start of DRAM, we've
 *  made KZERO 0xc0000000.  We've also direct mapped
 *  the special use io space between 0x8000000 and KZERO.
 *  That means we can leave the MMU off till we're pretty
 *  far into the boot.
 *
 *  Both these decisions can be changed by putting an MMU setup in
 *  l.s and changing the definitions below.
 */
#define	UZERO		0			/* base of user address space */
#define	UTZERO		(UZERO+BY2PG)		/* first address in user text */
#define	KZERO		0xC0000000		/* base of kernel address space */
#define	KTZERO		0xC0008000		/* first address in kernel text */
#define	IOZERO		0x80000000		/* 1 gig of special regs */
#define	USTKTOP		(IOZERO-BY2PG)		/* byte just beyond user stack */
#define	USTKSIZE	(16*1024*1024)		/* size of user stack */
#define	TSTKTOP		(USTKTOP-USTKSIZE)	/* end of new stack in sysexec */
#define TSTKSIZ 	100

#define KSTACK		(16*1024)		/* Size of kernel stack */

/*
 *  virtual MMU
 */
#define PTEMAPMEM	(1024*1024)	
#define	PTEPERTAB	(PTEMAPMEM/BY2PG)
#define SEGMAPSIZE	1984
#define SSEGMAPSIZE	16
#define PPN(x)		((x)&~(BY2PG-1))

/*
 *  physical MMU
 */
#define	PTEVALID	0
#define	PTERONLY	0
#define	PTEWRITE	0
#define	PTEUNCACHED	0

