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
	ulong	sp0;		/* pl0 stack pointer */
	ulong	ss0;		/* pl0 stack selector */
	ulong	sp1;		/* pl1 stack pointer */
	ulong	ss1;		/* pl1 stack selector */
	ulong	sp2;		/* pl2 stack pointer */
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
Tss tss;

/*
 *  segment descriptor initializers
 */
#define	DATASEGM(p) 	{ 0xFFFF, SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	EXECSEGM(p) 	{ 0xFFFF, SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define CALLGATE(s,o,p)	{ ((o)&0xFFFF)|((s)<<16), (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGCG }
#define	D16SEGM(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	E16SEGM(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define	TSSSEGM(b,p)	{ ((b)<<16)|sizeof(Tss),\
			  ((b)&0xFF000000)|(((b)<<16)&0xFF)|SEGTSS|SEGPL(p)|SEGP }

/*
 *  global descriptor table describing all segments
 */
Segdesc gdt[] =
{
[NULLSEG]	{ 0, 0},		/* null descriptor */
[KDSEG]		DATASEGM(0),		/* kernel data/stack */
[KESEG]		EXECSEGM(0),		/* kernel code */
[UDSEG]		DATASEGM(3),		/* user data/stack */
[UESEG]		EXECSEGM(3),		/* user code */
[SYSGATE]	CALLGATE(KESEL,0,3),	/* call gate for system calls */
[TSSSEG]	TSSSEGM(0,0),		/* tss segment */
};

static ulong	*ktoppt;	/* prototype top level page table
				 * containing kernel mappings
				 */
static ulong	*toppt;		/* top level page table */	
static ulong	*kpt;		/* kernel level page tables */
static ulong	*upt;		/* page table for struct User */

#define ROUNDUP(s,v)	(((s)+(v-1))&~(v-1))
/*
 *  offset of virtual address into
 *  top level page table
 */
#define TOPOFF(v)	((v)>>(2*PGSHIFT-2))

/*
 *  offset of virtual address into
 *  bottom level page table
 */
#define BTMOFF(v)	(((v)>>(PGSHIFT))&(WD2PG-1))

void
mmudump(void)
{
	int i;
	ulong *z;
	z = (ulong*)gdt;
	for(i = 0; i < sizeof(gdt)/4; i+=2)
		print("%8.8lux %8.8lux\n", *z++, *z++);
	print("UESEL %lux UDSEL %lux\n", UESEL, UDSEL);
	print("KESEL %lux KDSEL %lux\n", KESEL, KDSEL);
	panic("done");
}

void
mmuinit(void)
{
	int i, nkpt, npage, nbytes;
	ulong x;
	ulong y;

	/*
	 *  set up the global descriptor table
	 */
	x = (ulong)systrap;
	gdt[SYSGATE].d0 = (x&0xFFFF)|(KESEL<<16);
	gdt[SYSGATE].d1 = (x&0xFFFF0000)|SEGP|SEGPL(3)|SEGCG;
	x = (ulong)&tss;
	gdt[TSSSEG].d0 = (x<<16)|sizeof(Tss);
	gdt[TSSSEG].d1 = (x&0xFF000000)|((x>>16)&0xFF)|SEGTSS|SEGPL(0)|SEGP;
	putgdt(gdt, sizeof gdt);

	/*
	 *  set up system page tables.
	 *  map all of physical memory to start at KZERO.
	 *  leave a map for a user area.
	 */

	/*  allocate and fill low level page tables for kernel mem */
	npage = conf.base1/BY2PG + conf.npage1;
	nbytes = PGROUND(npage*BY2WD);		/* words of page map */
	nkpt = nbytes/BY2PG;			/* pages of page map */
	kpt = ialloc(nbytes, 1);
	for(i = 0; i < npage; i++)
		kpt[i] = (i<<PGSHIFT) | PTEVALID | PTEKERNEL | PTEWRITE;

print("%d low level pte's, %d high level pte's\n", npage, nkpt);

	/*  allocate page table for u-> */
	upt = ialloc(BY2PG, 1);

	/*  allocate top level table and put pointers to lower tables in it */
	toppt = ialloc(BY2PG, 1);
	x = TOPOFF(KZERO);
	y = ((ulong)kpt)&~KZERO;
	for(i = 0; i < nkpt; i++)
		toppt[x+i] = (y+i*BY2PG) | PTEVALID | PTEKERNEL | PTEWRITE;
	x = TOPOFF(USERADDR);
	y = ((ulong)upt)&~KZERO;
	toppt[x] = y | PTEVALID | PTEKERNEL | PTEWRITE;
	putcr3(((ulong)toppt)&~KZERO);

	/*
	 *  set up the task segment
	 */
	tss.sp0 = USERADDR+BY2PG;
	tss.ss0 = KDSEL;
	tss.cr3 = (ulong)toppt;
	puttr(TSSSEL);
}


void
mapstack(Proc *p)
{
	ulong tlbphys;
	int i;

	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)) && p->pid != 0)
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);

	/*
 	 *  dump any invalid mappings
	 */
	if(p->mmuvalid == 0){
		for(i = 0; i < MAXMMU+MAXSMMU; i++){
			if(p->mmu[i]==0)
				continue;
			memset(kmap(p->mmu[i]), 0, BY2PG);
		}
		p->mmuvalid = 1;
	}

	/*
	 *  point top level page table to bottom level ones
	 */
	memmove(toppt, p->mmue, MAXMMU*sizeof(ulong));
	memmove(&toppt[TOPOFF(USTKBTM)], &p->mmue[MAXMMU], MAXSMMU*sizeof(ulong));

	/* map in u area */
	upt[0] = PPN(p->upage->pa) | PTEVALID | PTEKERNEL | PTEWRITE;

	/* flush cached mmu entries */
	putcr3(((ulong)toppt)&~KZERO);

	u = (User*)USERADDR;
}

void
flushmmu(void)
{
	int s;

	if(u == 0)
		return;

	u->p->mmuvalid = 0;
	s = splhi();
	mapstack(u->p);
	splx(s);
}

void
mmurelease(Proc *p)
{
	p->mmuvalid = 0;
}

void
putmmu(ulong va, ulong pa, Page *pg)
{
	int topoff;
	ulong *pt;
	Proc *p;
	int i = 0;

/*print("putmmu %lux %lux\n", va, pa); /**/
	if(u==0)
		panic("putmmu");
	p = u->p;

	/*
	 *  check for exec/data vs stack vs illegal
	 */
	topoff = TOPOFF(va);
	if(topoff < TOPOFF(TSTKTOP) && topoff >= TOPOFF(USTKBTM))
		i = MAXMMU + topoff - TOPOFF(USTKBTM);
	else if(topoff < MAXMMU)
		i = topoff;
	else
		panic("putmmu bad addr %lux", va);

	/*
	 *  if bottom level page table missing, allocate one
	 */
	pg = p->mmu[i];
/*print("toppt[%d] was %lux\n", topoff, toppt[topoff]);/**/
	if(pg == 0){
		pg = p->mmu[i] = newpage(1, 0, 0);
		p->mmue[i] = PPN(pg->pa) | PTEVALID | PTEUSER | PTEWRITE;
		toppt[topoff] = p->mmue[i];
/*print("toppt[%d] now %lux\n", topoff, toppt[topoff]);/**/
	}

	/*
	 *  fill in the bottom level page table
	 */
	pt = (ulong*)(p->mmu[i]->pa|KZERO);
/*print("%lux[%d] was %lux\n", pt, BTMOFF(va), pt[BTMOFF(va)]);/**/
	pt[BTMOFF(va)] = pa | PTEUSER;
/*print("%lux[%d] now %lux\n", pt, BTMOFF(va), pt[BTMOFF(va)]);/**/

	/* flush cached mmu entries */
	putcr3(((ulong)toppt)&~KZERO);
}

void
invalidateu(void)
{
	/* unmap u area */
	upt[0] = 0;

	/* flush cached mmu entries */
	putcr3(((ulong)toppt)&~KZERO);
}

void
systrap(void)
{
	panic("system trap from user");
}
