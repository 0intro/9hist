#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

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

/*
 *  segment descriptor initializers
 */
#define	DATASEG(p) 	{ 0xFFFF, SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	EXECSEG(p) 	{ 0xFFFF, SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define CALLGATE(s,o,p)	{ (o)&0xFFFF|((s)<<16), (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGCG }
#define	D16SEG(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	E16SEG(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }

/*
 *  global descriptor table describing all segments
 */
Segdesc gdt[] =
{
[NULLSEG]	{ 0, 0},		/* null descriptor */
[KDSEG]		DATASEG(0),		/* kernel data/stack */
[KESEG]		EXECSEG(0),		/* kernel code */
[UDSEG]		DATASEG(3),		/* user data/stack */
[UESEG]		EXECSEG(3),		/* user code */
[SYSGATE]	CALLGATE(KESEL,0,3),	/* call gate for system calls */
[RDSEG]		D16SEG(0),		/* reboot data/stack */
[RESEG]		E16SEG(0),		/* reboot code */
};

void
mmuinit(void)
{
	gdt[SYSGATE].d0 = ((ulong)systrap)&0xFFFF|(KESEL<<16);
	gdt[SYSGATE].d1 = ((ulong)systrap)&0xFFFF0000|SEGP|SEGPL(3)|SEGCG;
	lgdt(gdt, sizeof gdt);
}

void
systrap(void)
{
	panic("system trap from user");
}
