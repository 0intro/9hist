#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#define	DATASEGM(p) 	{ 0xFFFF, SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	EXECSEGM(p) 	{ 0xFFFF, SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define	TSSSEGM(b,p)	{ ((b)<<16)|sizeof(Tss),\
			  ((b)&0xFF000000)|(((b)>>16)&0xFF)|SEGTSS|SEGPL(p)|SEGP }

Segdesc gdt[6] =
{
[NULLSEG]	{ 0, 0},		/* null descriptor */
[KDSEG]		DATASEGM(0),		/* kernel data/stack */
[KESEG]		EXECSEGM(0),		/* kernel code */
[UDSEG]		DATASEGM(3),		/* user data/stack */
[UESEG]		EXECSEGM(3),		/* user code */
[TSSSEG]	TSSSEGM(0,0),		/* tss segment */
};

#define PDX(va)		((((ulong)(va))>>22) & 0x03FF)
#define PTX(va)		((((ulong)(va))>>12) & 0x03FF)

static int ptebits = 0;

static void
taskswitch(ulong pagetbl, ulong stack)
{
	Tss *tss;

	tss = m->tss;
	tss->ss0 = KDSEL;
	tss->esp0 = stack;
	tss->ss1 = KDSEL;
	tss->esp1 = stack;
	tss->ss2 = KDSEL;
	tss->esp2 = stack;
	tss->cr3 = pagetbl;
	putcr3(pagetbl);
}

void
mmuinit(void)
{
	ulong x;
	ushort ptr[3];

	m->tss = malloc(sizeof(Tss));
	memset(m->tss, 0, sizeof(Tss));

	memmove(m->gdt, gdt, sizeof(m->gdt));
	x = (ulong)m->tss;
	m->gdt[TSSSEG].d0 = (x<<16)|sizeof(Tss);
	m->gdt[TSSSEG].d1 = (x&0xFF000000)|((x>>16)&0xFF)|SEGTSS|SEGPL(0)|SEGP;

	ptr[0] = sizeof(m->gdt);
	x = (ulong)m->gdt;
	ptr[1] = x & 0xFFFF;
	ptr[2] = (x>>16) & 0xFFFF;
	lgdt(ptr);

	ptr[0] = sizeof(Segdesc)*256;
	x = IDTADDR;
	ptr[1] = x & 0xFFFF;
	ptr[2] = (x>>16) & 0xFFFF;
	lidt(ptr);

	taskswitch(PADDR(m->pdb),  (ulong)m + BY2PG);
	ltr(TSSSEL);
}

ulong*
mmuwalk(ulong *pdb, ulong va, int create)
{
	ulong *table, x;

	table = &pdb[PDX(va)];
	if(*table == 0){
		if(create == 0)
			return 0;
		x = PADDR((ulong)xspanalloc(BY2PG, BY2PG, 0));
		*table = x|ptebits|PTEWRITE|PTEVALID;
	}
	table = (ulong*)(KZERO|PPN(*table));
	va = PTX(va);
	return &table[va];
}

void
flushmmu(void)
{
	int s;

	s = splhi();
	up->newtlb = 1;
	mmuswitch(up);
	splx(s);
}

static void
mmuptefree(Proc *p)
{
	ulong *pdb;
	Page **lpg, *pg;

	if(p->mmupdb && p->mmuused){
		pdb = (ulong*)p->mmupdb->va;
		lpg = &p->mmuused;
		for(pg = *lpg; pg; pg = pg->next){
			pdb[pg->daddr] = 0;
			lpg = &pg->next;
		}
		*lpg = p->mmufree;
		p->mmufree = p->mmuused;
		p->mmuused = 0;
	}
}

void
mmuswitch(Proc *p)
{
	ulong *top;

	if(p->newtlb){
		mmuptefree(p);
		p->newtlb = 0;
	}

	if(p->mmupdb){
		top = (ulong*)p->mmupdb->va;
		top[PDX(MACHADDR)] = ((ulong*)m->pdb)[PDX(MACHADDR)];
		taskswitch(p->mmupdb->pa, (ulong)(p->kstack+KSTACK));
	}
	else
		taskswitch(PADDR(m->pdb), (ulong)(p->kstack+KSTACK));
}

void
mmurelease(Proc *p)
{
	Page *pg, *next;

	/*
	 * Release any pages allocated for a page directory base or page-tables
	 * for this process:
	 *   switch to the prototype pdb for this processor (m->pdb);
	 *   call mmuptefree() to place all pages used for page-tables (p->mmuused)
	 *   onto the process' free list (p->mmufree). This has the side-effect of
	 *   cleaning any user entries in the pdb (p->mmupdb);
	 *   if there's a pdb put it in the cache of pre-initialised pdb's
	 *   for this processor (m->pdbpool) or on the process' free list;
	 *   finally, place any pages freed back into the free pool (palloc).
	 * This routine is only called from sched() with palloc locked.
	 */
	taskswitch(PADDR(m->pdb), (ulong)m + BY2PG);
	mmuptefree(p);

	if(p->mmupdb){
		if(m->pdbcnt > 10){
			p->mmupdb->next = p->mmufree;
			p->mmufree = p->mmupdb;
		}
		else{
			p->mmupdb->next = m->pdbpool;
			m->pdbpool = p->mmupdb;
			m->pdbcnt++;
		}
		p->mmupdb = 0;
	}

	for(pg = p->mmufree; pg; pg = next){
		next = pg->next;
		if(--pg->ref)
			panic("mmurelease: pg->ref %d\n", pg->ref);
		pg->ref = 0;
		if(palloc.head){
			pg->next = palloc.head;
			palloc.head->prev = pg;
		}
		else{
			palloc.tail = pg;
			pg->next = 0;
		}
		palloc.head = pg;
		pg->prev = 0;

		palloc.freecount++;
	}
	if(p->mmufree && palloc.r.p)
		wakeup(&palloc.r);
	p->mmufree = 0;
}

static Page*
mmupdballoc(void)
{
	int s;
	Page *pg;

	s = splhi();
	if(m->pdbpool == 0){
		spllo();
		pg = newpage(0, 0, 0);
		pg->va = VA(kmap(pg));
		memmove((void*)pg->va, m->pdb, BY2PG);
	}
	else{
		pg = m->pdbpool;
		m->pdbpool = pg->next;
		m->pdbcnt--;
	}
	splx(s);
	return pg;
}

void
putmmu(ulong va, ulong pa, Page *pg)
{
	int pdbx;
	ulong *pdb, *pt;
	int s;

	if(up->mmupdb == 0)
		up->mmupdb = mmupdballoc();
	pdb = (ulong*)up->mmupdb->va;
	pdbx = PDX(va);

	if(PPN(pdb[pdbx]) == 0){
		if(up->mmufree == 0){
			pg = newpage(1, 0, 0);
			pg->va = VA(kmap(pg));
		}
		else {
			pg = up->mmufree;
			up->mmufree = pg->next;
			memset((void*)pg->va, 0, BY2PG);
		}
		pdb[pdbx] = PPN(pg->pa)|ptebits|PTEUSER|PTEWRITE|PTEVALID;
		pg->daddr = pdbx;
		pg->next = up->mmuused;
		up->mmuused = pg;
	}

	pt = (ulong*)(PPN(pdb[pdbx])|KZERO);
	pt[PTX(va)] = pa|ptebits|PTEUSER;

	s = splhi();
	pdb[PDX(MACHADDR)] = ((ulong*)m->pdb)[PDX(MACHADDR)];
	putcr3(up->mmupdb->pa);
	splx(s);
}
