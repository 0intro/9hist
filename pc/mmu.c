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
mmuptefree(Proc* p)
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
mmuswitch(Proc* p)
{
	ulong *top;

	if(p->newtlb){
		mmuptefree(p);
		p->newtlb = 0;
	}

	if(p->mmupdb){
		top = (ulong*)p->mmupdb->va;
		top[PDX(MACHADDR)] = m->pdb[PDX(MACHADDR)];
		taskswitch(p->mmupdb->pa, (ulong)(p->kstack+KSTACK));
	}
	else
		taskswitch(PADDR(m->pdb), (ulong)(p->kstack+KSTACK));
}

void
mmurelease(Proc* p)
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
putmmu(ulong va, ulong pa, Page* pg)
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
		pdb[pdbx] = PPN(pg->pa)|PTEUSER|PTEWRITE|PTEVALID;
		pg->daddr = pdbx;
		pg->next = up->mmuused;
		up->mmuused = pg;
	}

	pt = KADDR(PPN(pdb[pdbx]));
	pt[PTX(va)] = pa|PTEUSER;

	s = splhi();
	pdb[PDX(MACHADDR)] = m->pdb[PDX(MACHADDR)];
	mmuflushtlb(up->mmupdb->pa);
	splx(s);
}

ulong*
mmuwalk(ulong* pdb, ulong va, int level, int create)
{
	ulong pa, *table;

	/*
	 * Walk the page-table pointed to by pdb and return a pointer
	 * to the entry for virtual address va at the requested level.
	 * If the entry is invalid and create isn't requested then bail
	 * out early. Otherwise, for the 2nd level walk, allocate a new
	 * page-table page and register it in the 1st level.
	 */
	table = &pdb[PDX(va)];
	if(!(*table & PTEVALID) && create == 0)
		return 0;

	switch(level){

	default:
		return 0;

	case 1:
		return table;

	case 2:
		if(*table & PTESIZE)
			panic("mmuwalk2: va %uX entry %uX\n", va, *table);
		if(!(*table & PTEVALID)){
			pa = PADDR(xspanalloc(BY2PG, BY2PG, 0));
			*table = pa|PTEWRITE|PTEVALID;
		}
		table = KADDR(PPN(*table));

		return &table[PTX(va)];
	}
}

static Lock mmukmaplock;

int
mmukmapsync(ulong va)
{
	Mach *mach0;
	ulong entry, *pte;

	mach0 = MACHP(0);

	lock(&mmukmaplock);

	if((pte = mmuwalk(mach0->pdb, va, 1, 0)) == nil){
		unlock(&mmukmaplock);
		return 0;
	}
	if(!(*pte & PTESIZE) && mmuwalk(mach0->pdb, va, 2, 0) == nil){
		unlock(&mmukmaplock);
		return 0;
	}
	entry = *pte;

	if(!(m->pdb[PDX(va)] & PTEVALID))
		m->pdb[PDX(va)] = entry;

	if(up && up->mmupdb){
		((ulong*)up->mmupdb->va)[PDX(va)] = entry;
		mmuflushtlb(up->mmupdb->pa);
	}
	else
		mmuflushtlb(PADDR(m->pdb));

	unlock(&mmukmaplock);

	return 1;
}

ulong
mmukmap(ulong pa, ulong va, int size)
{
	Mach *mach0;
	ulong ova, pae, *table, pgsz, *pte, x;
	int pse, sync;

	mach0 = MACHP(0);
	if((mach0->cpuiddx & 0x08) && (getcr4() & 0x10))
		pse = 1;
	else
		pse = 0;
	sync = 0;

	pa = PPN(pa);
	if(va == 0)
		va = (ulong)KADDR(pa);
	else
		va = PPN(va);
	ova = va;

	pae = pa + size;
	lock(&mmukmaplock);
	while(pa < pae){
		table = &mach0->pdb[PDX(va)];
		/*
		 * Possibly already mapped.
		 */
		if(*table & PTEVALID){
			if(*table & PTESIZE){
				/*
				 * Big page. Does it fit within?
				 * If it does, adjust pgsz so the correct end can be
				 * returned and get out.
				 * If not, adjust pgsz up to the next 4MB boundary
				 * and continue.
				 */
				x = PPN(*table);
				if(x != pa)
					panic("mmukmap1: pa %ux  entry %uX\n",
						pa, *table);
				x += 4*MB;
				if(pae <= x){
					pa = pae;
					break;
				}
				pgsz = x - pa;
				pa += pgsz;
				va += pgsz;

				continue;
			}
			else{
				/*
				 * Little page. Walk to the entry.
				 * If the entry is valid, set pgsz and continue.
				 * If not, make it so, set pgsz, sync and continue.
				 */
				pte = mmuwalk(mach0->pdb, va, 2, 0);
				if(pte && *pte & PTEVALID){
					x = PPN(*pte);
					if(x != pa)
						panic("mmukmap2: pa %ux entry %uX\n",
							pa, *pte);
					pgsz = BY2PG;
					pa += pgsz;
					va += pgsz;
					sync++;

					continue;
				}
			}
		}

		/*
		 * Not mapped. Check if it can be mapped using a big page -
		 * starts on a 4MB boundary, size >= 4MB and processor can do it.
		 * If not a big page, walk the walk, talk the talk.
		 * Sync is set.
		 */
		if(pse && (pa % (4*MB)) == 0 && (pae >= pa+4*MB)){
			*table = pa|PTESIZE|PTEWRITE|PTEUNCACHED|PTEVALID;
			pgsz = 4*MB;
		}
		else{
			pte = mmuwalk(mach0->pdb, va, 2, 1);
			*pte = pa|PTEWRITE|PTEUNCACHED|PTEVALID;
			pgsz = BY2PG;
		}
		pa += pgsz;
		va += pgsz;
		sync++;
	}
	unlock(&mmukmaplock);

	/*
	 * If something was added
	 * then need to sync up.
	 */
	if(sync)
		mmukmapsync(ova);

	return pa;
}
