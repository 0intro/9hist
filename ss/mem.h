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
#define	PGSHIFT		12			/* log(BY2PG) */

#define	MAXMACH		1			/* max # cpus system can run */

/*
 * Time
 */
#define	HZ		(60)			/* clock frequency */
#define	MS2HZ		(1000/HZ)		/* millisec per clock tick */
#define	TK2SEC(t)	((t)/HZ)		/* ticks to seconds */
#define	TK2MS(t)	((((ulong)(t))*1000)/HZ)	/* ticks to milliseconds */
#define	MS2TK(t)	((((ulong)(t))*HZ)/1000)	/* milliseconds to ticks */

/*
 * SR bits
 */
#define SUPER		0x2000
#define SPL(n)		(n<<8)

/*
 * CACR
 */
#define	CCLEAR		0x08
#define	CENABLE		0x01

/*
 * Magic registers
 */

#define	MACH		28		/* R28 is m-> */
#define	USER		27		/* R27 is u-> */

/*
 * Fundamental addresses
 */

#define	USERADDR	0x80000000
#define	UREGADDR	(USERADDR+BY2PG-(2+4+2+(8+8+1)*BY2WD))

/*
 * Devices poked during bootstrap
 */
#define	TACADDR		0x40600000
#define	MOUSE		0x40200000

/*
 * MMU
 */

#define	VAMASK	0x1FFFFFFF

/*
 * MMU entries
 */
#define	PTEVALID	(1<<31)
#define	PTEWRITE	(1<<30)
#define	PTEKERNEL	(1<<29)
#define	PTENOCACHE	(1<<28)
#define	PTEMAINMEM	(0<<26)
#define	PTEIO		(1<<26)
#define	PTEACCESS	(1<<25)
#define	PTEMODIFY	(1<<24)

#define	PTERONLY	0		/* BUG */
#define	INVALIDPTE	0
#define	PPN(pa)		((pa>>12)&0xFFFF)

#define	KMAP	((unsigned long *)0xD0000000)
#define	UMAP	((unsigned long *)0x50000000)

/*
 * Virtual addresses
 */
#define	VTAG(va)	((va>>22)&0x03F)
#define	VPN(va)		((va>>13)&0x1FF)

#define	PARAM		((char*)0x40500000)
#define	TLBFLUSH_	0x01

/*
 * Address spaces
 */

#define	UZERO	0x00000000			/* base of user address space */
#define	UTZERO	(UZERO+BY2PG)		/* first address in user text */
#define	TSTKTOP	0x10000000		/* end of new stack in sysexec */
#define	USTKTOP	(TSTKTOP-100*BY2PG)	/* byte just beyond user stack */
#define	KZERO	0x10000000		/* base of kernel address space */
#define	KTZERO	(KZERO+4*BY2PG)		/* first address in kernel text */
#define	USTACKSIZE	(4*1024*1024)	/* size of user stack */

#define	NSEG		5
#define	MACHSIZE	4096
