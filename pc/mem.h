#define SELGDT	(0<<3)	/* selector is in gdt */
#define	SELLDT	(1<<3)	/* selector is in ldt */

#define SELECTOR(i, t, p)	(((i)<<4) | (t) | (p))

/*
 *  segment descriptor/gate
 */
typedef struct Segdesc	Segdesc;
struct Segdesc
{
	ulong	d0;
	ulong	d1;
};
#define SEGDATA	(0x10<<8)	/* data/stack segment */
#define SEGEXEC	(0x18<<8)	/* executable segment */
#define SEGCG	(0x0C<<8)	/* call gate */
#define	SEGIG	(0x0E<<8)	/* interrupt gate */
#define SEGTG	(0x0F<<8)	/* task gate */

#define SEGP	(1<<15)		/* segment present */
#define SEGPL(x) ((x)<<13)	/* priority level */
#define SEGB	(1<<22)		/* granularity 1==4k (for expand-down) */
#define SEGG	(1<<23)		/* granularity 1==4k (for other) */
#define SEGE	(1<<10)		/* expand down */
#define SEGW	(1<<9)		/* writable (for data/stack) */
#define	SEGR	(1<<9)		/* readable (for code) */
#define SEGD	(1<<22)		/* default 1==32bit (for code) */

/*
 *  gate initializers
 */
#define TRAPGATE(s,o,p)	{ (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGTG, (o)&0xFFFF|((s)<<16) }
#define INTRGATE(s,o,p)	{ (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGIG, (o)&0xFFFF|((s)<<16) }
#define CALLGATE(s,o,p)	{ (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGCG, (o)&0xFFFF|((s)<<16) }

/*
 *  segment descriptor initializers
 */
#define	DATASEG(p) 	{ SEGG|(0xF<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW, 0xFFFF }
#define	EXECSEG(p) 	{ SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR, 0xFFFF }

/*
 *  known segments (in GDT) and their selectors
 */
enum
{
	NULLSEG=0,	/* null segment */
	KESEG=	1,	/* kernel executable */	
	KDSEG=	2,	/* kernel data/stack */
	UESEG=	3,	/* user executable */
	UDSEG=	4,	/* user data/stack */
	SYSGATE=5,	/* system call gate */
};
#define NULLSEL	SELECTOR(NULLSEG, SELGDT, 0)
#define KESEL	SELECTOR(KESEG, SELGDT, 0)
#define KDSEL	SELECTOR(KDSEG, SELGDT, 0)
#define KSSEL	SELECTOR(KDSEG, SELGDT, 0)
#define UESEL	SELECTOR(UESEG, SELGDT, 3)
#define UDSEL	SELECTOR(UDSEG, SELGDT, 3)
#define USSEL	SELECTOR(UDSEG, SELGDT, 3)

/*
 *  task state segment.  Plan 9 ignores all the task switching goo and just
 *  uses the tss for esp0 and ss0 on gate's into the kernel, interrupts,
 *  and exceptions.  The rest is completely ignored.
 *
 *  This means that we only need one tss in the whole system.
 */
typedef struct Tss	Tss;
struct Tss
{
	ulong	backlink;	/* unused */
	ulong	esp0;		/* pl0 stack pointer */
	ulong	ss0;		/* pl0 stack selector */
	ulong	esp1;		/* pl1 stack pointer */
	ulong	ss1;		/* pl1 stack selector */
	ulong	esp2;		/* pl2 stack pointer */
	ulong	ss2;		/* pl2 stack selector */
	ulong	cr3;		/* page table descriptor */
	ulong	eip;		/* instruction pointer */
	ulong	eflags;		/* processor flags */
	ulong	eax;		/* general (hah?) registers */
	ulong 	ecx;
	ulong	edx;
	ulong	ebx;
	ulong	esp;
	ulong	ebp;
	ulong	esi;
	ulong	edi;
	ulong	es;		/* segment selectors */
	ulong	cs;
	ulong	ss;
	ulong	ds;
	ulong	fs;
	ulong	gs;
	ulong	ldt;		/* local descriptor table */
	ulong	iomap;		/* io map base */
};
