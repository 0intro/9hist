/*
 * Memory and machine-specific definitions.  Used in C and assembler.
 */

/*
 * Sizes
 */
#define	BI2BY		8			/* bits per byte */
#define BI2WD		32			/* bits per word */
#define	BY2WD		4			/* bytes per word */
#define	BY2PG		4096			/* bytes per page */
#define	WD2PG		(BY2PG/BY2WD)		/* words per page */
#define	PGSHIFT		13			/* log(BY2PG) */
#define PGROUND(s)	(((s)+(BY2PG-1))&~(BY2PG-1))

#define	MAXMACH		1			/* max # cpus system can run */

/*
 * Time
 * Clock frequency is ??? HZ
 */
#define	HZ		(18)			/* clock frequency */
#define	MS2HZ		(54)			/* millisec per clock tick */
#define	TK2SEC(t)	((t)*10/185)		/* ticks to seconds */
#define	TK2MS(t)	((((ulong)(t))*10000)/185)	/* ticks to milliseconds */
#define	MS2TK(t)	((((ulong)(t))*185)/10000)	/* milliseconds to ticks */

/*
 * Fundamental addresses
 */
#define	USERADDR	0xC0000000
#define	UREGADDR	(USERADDR+BY2PG-4*16)

/*
 * Address spaces
 *
 * User is at 0-2GB
 * Kernel is at 2GB-4GB
 */
#define	UZERO	0			/* base of user address space */
#define	UTZERO	(UZERO+BY2PG)		/* first address in user text */
#define	TSTKTOP	USERADDR		/* end of new stack in sysexec */
#define TSTKSIZ 10
#define	USTKTOP	(TSTKTOP-TSTKSIZ*BY2PG)	/* byte just beyond user stack */
#define	KZERO	0x80000000		/* base of kernel address space */
#define	KTZERO	KZERO			/* first address in kernel text */
#define	USTKSIZE	(4*1024*1024)	/* size of user stack */

#define	MACHSIZE	4096

#define isphys(x) ((x)&KZERO)

/*
 *  known 80386 segments (in GDT) and their selectors
 */
#define	NULLSEG	0	/* null segment */
#define	KDSEG	1	/* kernel data/stack */
#define	KESEG	2	/* kernel executable */	
#define	UDSEG	3	/* user data/stack */
#define	UESEG	4	/* user executable */
#define	SYSGATE	5	/* system call gate */
#define	RDSEG	6	/* reboot data/stack */
#define	RESEG	7	/* reboot executable */	

#define SELGDT	(0<<3)	/* selector is in gdt */
#define	SELLDT	(1<<3)	/* selector is in ldt */

#define SELECTOR(i, t, p)	(((i)<<3) | (t) | (p))

#define NULLSEL	SELECTOR(NULLSEG, SELGDT, 0)
#define KESEL	SELECTOR(KESEG, SELGDT, 0)
#define KDSEL	SELECTOR(KDSEG, SELGDT, 0)
#define KSSEL	SELECTOR(KDSEG, SELGDT, 0)
#define UESEL	SELECTOR(UESEG, SELGDT, 3)
#define UDSEL	SELECTOR(UDSEG, SELGDT, 3)
#define USSEL	SELECTOR(UDSEG, SELGDT, 3)
#define RDSEL	SELECTOR(RDSEG, SELGDT, 0)
#define RESEL	SELECTOR(RESEG, SELGDT, 0)

/*
 *  fields in segment descriptors
 */
#define SEGDATA	(0x10<<8)	/* data/stack segment */
#define SEGEXEC	(0x18<<8)	/* executable segment */
#define SEGCG	(0x0C<<8)	/* call gate */
#define	SEGIG	(0x0E<<8)	/* interrupt gate */
#define SEGTG	(0x0F<<8)	/* task gate */
#define SEGTYPE	(0x1F<<8)

#define SEGP	(1<<15)		/* segment present */
#define SEGPL(x) ((x)<<13)	/* priority level */
#define SEGB	(1<<22)		/* granularity 1==4k (for expand-down) */
#define SEGG	(1<<23)		/* granularity 1==4k (for other) */
#define SEGE	(1<<10)		/* expand down */
#define SEGW	(1<<9)		/* writable (for data/stack) */
#define	SEGR	(1<<9)		/* readable (for code) */
#define SEGD	(1<<22)		/* default 1==32bit (for code) */

/*
 *  virtual MMU entries
 */
#define PTEMAPMEM	(1024*1024)	/* ??? */	
#define SEGMAPSIZE	16		/* ??? */
#define	PTEPERTAB	(PTEMAPMEM/BY2PG)	/* ??? */
