#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

struct
{
	Lock;
	int	init;
	KMap	*free;
	KMap	arena[4*1024*1024/BY2PG];	/* kernel mmu maps up to 4MB */
}kmapalloc;

void
putxmmu(ulong tlbvirt, ulong tlbphys, int pid)
{
	if(pid != u->p->pid)
		panic("putxmmu %ld %ld\n", pid, u->p->pid);
	if(tlbvirt&KZERO)
		panic("putmmu");
	tlbphys |= VTAG(tlbvirt)<<24;
	UMAP[(tlbvirt&0x003FE000L)>>2] = tlbphys;
}

/*
 * Called splhi, not in Running state
 */
void
mapstack(Proc *p)
{
	ulong tlbvirt, tlbphys;
	ulong i;

	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)))
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);
	tlbvirt = USERADDR;
	tlbphys = PPN(p->upage->pa) | PTEVALID | PTEKERNEL;
	putkmmu(tlbvirt, tlbphys);
	flushmmu();
	u = (User*)USERADDR;

	/*
 	 *  preload the MMU with the last (up to) NMMU user entries
	 *  previously faulted into it for this process.
	 */
	if(u->mc.next >= NMMU){
		u->mc.next &= NMMU - 1;
		for(i = u->mc.next; i < NMMU; i++)
			putxmmu(u->mc.mmu[i].va, u->mc.mmu[i].pa, u->mc.mmu[i].pid);
	}
	for(i = 0; i < u->mc.next; i++)
		putxmmu(u->mc.mmu[i].va, u->mc.mmu[i].pa, u->mc.mmu[i].pid);
}

void
putkmmu(ulong tlbvirt, ulong tlbphys)
{
	if(!(tlbvirt&KZERO))
		panic("putkmmu");
	tlbvirt &= ~KZERO;
	KMAP[(tlbvirt&0x003FE000L)>>2] = tlbphys;
}

void
putmmu(ulong tlbvirt, ulong tlbphys)
{
	if(tlbvirt&KZERO)
		panic("putmmu");
	if(u){
		MMU *mp;
		mp = &(u->mc.mmu[u->mc.next&(NMMU-1)]);
		mp->pa = tlbphys;
		mp->va = tlbvirt;
		mp->pid = u->p->pid;
		u->mc.next++;
	}
	tlbphys |= VTAG(tlbvirt)<<24;
	UMAP[(tlbvirt&0x003FE000L)>>2] = tlbphys;
}

void
flushmmu(void)
{
	flushcpucache();
	*PARAM &= ~TLBFLUSH_;
	*PARAM |= TLBFLUSH_;
}

void
clearmmucache(void)
{
	if(u == 0)
		panic("flushmmucache");
	u->mc.next = 0;
}

void
kmapinit(void)
{
	KMap *k;
	int i, e;

	if(kmapalloc.init == 0){
		k = &kmapalloc.arena[0];
		k->va = KZERO|(4*1024*1024-256*1024-BY2PG);
		k->next = 0;
		kmapalloc.free = k;
		kmapalloc.init = 1;
		return;
	}
	e = (4*1024*1024 - 256*1024)/BY2PG;	/* screen lives at top 256K */
	i = (((ulong)ialloc(0, 0))&~KZERO)/BY2PG;
	print("%lud free map registers\n", e-i);
	kmapalloc.free = 0;
	for(k=&kmapalloc.arena[i]; i<e; i++,k++){
		k->va = i*BY2PG|KZERO;
		kunmap(k);
	}
}

KMap*
kmap(Page *pg)
{
	KMap *k;

	lock(&kmapalloc);
	k = kmapalloc.free;
	if(k == 0){
		dumpstack();
		panic("kmap");
	}
	kmapalloc.free = k->next;
	unlock(&kmapalloc);
	k->pa = pg->pa;
	putkmmu(k->va, PPN(k->pa) | PTEVALID | PTEKERNEL);
	return k;
}

void
kunmap(KMap *k)
{
	k->pa = 0;
	lock(&kmapalloc);
	k->next = kmapalloc.free;
	kmapalloc.free = k;
	putkmmu(k->va, INVALIDPTE);
	unlock(&kmapalloc);
}
