#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

struct
{
	Lock;
	int	init;
	int	lowpmeg;
	KMap	*free;
	KMap	arena[(IOEND-IOSEGM)/BY2PG];
}kmapalloc;

/*
 * On SPARC, tlbpid i == context i-1 so that 0 means unallocated
 */

int	newpid(Proc*);
void	purgepid(int);

/*
 * Called splhi, not in Running state
 */
void
mapstack(Proc *p)
{
	short tp;
	ulong tlbphys;

	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newpid(p);
		p->pidonmach[m->machno] = tp;
	}
	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)))
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);
	/* don't set m->pidhere[*tp] because we're only writing U entry */
	tlbphys = PPN(p->upage->pa)|PTEVALID|PTEWRITE|PTEKERNEL|PTEMAINMEM;
	putcontext(tp-1);
	putpmeg(USERADDR, tlbphys);
	u = (User*)USERADDR;
}

/*
 * Process must be non-interruptible
 */
int
newpid(Proc *p)
{
	int i, j;
	Proc *sp;

	i = m->lastpid+1;
	if(i >= NCONTEXT+1)
		i = 1;
	sp = m->pidproc[i];
	if(sp){
		sp->pidonmach[m->machno] = 0;
		purgepid(i);
	}
	m->pidproc[i] = p;
	m->lastpid = i;
print("new context %d\n", i);
	/*
	 * kludge: each context is allowed 2 pmegs, one for text and one for stack
	 */
	putsegm(UZERO, kmapalloc.lowpmeg+(2*i));
	putsegm(TSTKTOP-BY2SEGM, kmapalloc.lowpmeg+(2*i)+1);
	for(j=0; j<PG2SEGM; j++){
		putpmeg(UZERO+j*BY2PG, INVALIDPTE);
		putpmeg((TSTKTOP-BY2SEGM)+j*BY2PG, INVALIDPTE);
	}
	return i;
}

void
purgepid(int pid)
{
	int i, rpid;

	if(m->pidhere[pid] == 0)
		return;
print("purge pid %d\n", pid);
	memset(m->pidhere, 0, sizeof m->pidhere);
	putcontext(pid-1);
	/*
	 * Clear context from cache
	 */
	for(i=0; i<0x1000; i++)
		putwE((i<<4), 0);
print("purge done\n");
}


void
mmuinit(void)
{
	ulong l, i, j, c, pte;

	/*
	 * First map lots of memory as kernel addressable in all contexts
	 */
	i = 0;		/* used */
	for(c=0; c<NCONTEXT; c++)
		for(i=0; i<conf.maxialloc/BY2SEGM; i++)
			putcxsegm(c, KZERO+i*BY2SEGM, i);
	kmapalloc.lowpmeg = i;
	/*
	 * Make sure cache is turned on for kernel
	 */
	pte = PTEVALID|PTEWRITE|PTEKERNEL|PTEMAINMEM;
	for(i=0; i<conf.maxialloc/BY2PG; i++)
		putpmeg(KZERO+i*BY2PG, pte+i);
		

	/*
	 * Create invalid pmeg; use highest segment
	 */
	putsegm(INVALIDSEGM, INVALIDPMEG);
	for(i=0; i<PG2SEGM; i++)
		putpmeg(INVALIDSEGM+i*BY2PG, INVALIDPTE);
	for(c=0; c<NCONTEXT; c++){
		putcontext(c);
		putsegm(INVALIDSEGM, INVALIDPMEG);
		/*
		 * Invalidate user addresses
		 */
/*
		for(l=UZERO; l<KZERO; l+=BY2SEGM)
			putsegm(l, INVALIDPMEG);
*/
putsegm(0x0000, INVALIDPMEG);
		/*
		 * One segment for screen
		 */
		putsegm(SCREENSEGM, SCREENPMEG);
		if(c == 0){
			pte = PTEVALID|PTEWRITE|PTEKERNEL|PTENOCACHE|
				PTEIO|((DISPLAYRAM>>PGSHIFT)&0xFFFF);
			for(i=0; i<PG2SEGM; i++)
				putpmeg(SCREENSEGM+i*BY2PG, pte+i);
		}
		/*
		 * First page of IO space includes ROM; be careful
		 */
		putsegm(IOSEGM0, IOPMEG0);	/* IOSEGM == ROMSEGM */
		if(c == 0){
			pte = PTEVALID|PTEKERNEL|PTENOCACHE|
				PTEIO|((EPROM>>PGSHIFT)&0xFFFF);
			for(i=0; i<PG2ROM; i++)
				putpmeg(IOSEGM0+i*BY2PG, pte+i);
			for(; i<PG2SEGM; i++)
				putpmeg(IOSEGM0+i*BY2PG, INVALIDPTE);
		}
		/*
		 * Remaining segments for IO and kmap
		 */
		for(j=1; j<NIOSEGM; j++){
			putsegm(IOSEGM0+j*BY2SEGM, IOPMEG0+j);
			if(c == 0)
				for(i=0; i<PG2SEGM; i++)
					putpmeg(IOSEGM0+j*BY2SEGM+i*BY2PG, INVALIDPTE);
		}
	}
	putcontext(0);
}




void
putmmu(ulong tlbvirt, ulong tlbphys)
{
	short tp;
	Proc *p;

	splhi();
	p = u->p;
/*	if(p->state != Running)
		panic("putmmu state %lux %lux %s\n", u, p, statename[p->state]);
*/
	p->state = MMUing;
	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newpid(p);
		p->pidonmach[m->machno] = tp;
	}
	/*
	 * kludge part 2: make sure we've got a valid segment
	 */
	if(tlbvirt>=TSTKTOP || (UZERO+BY2SEGM<=tlbvirt && tlbvirt<(TSTKTOP-BY2SEGM)))
		panic("putmmu %lux", tlbvirt);
	putpmeg(tlbvirt, tlbphys);
	m->pidhere[tp] = 1;
	p->state = Running;
	spllo();
}

void
flushmmu(void)
{
	splhi();
	/* easiest is to forget what pid we had.... */
	memset(u->p->pidonmach, 0, sizeof u->p->pidonmach);
	/* ....then get a new one by trying to map our stack */
	mapstack(u->p);
	spllo();
}

void
cacheinit(void)
{
	int i;

	/*
	 * Initialize cache by clearing the valid bit
	 * (along with the others) in all cache entries
	 */
	for(i=0; i<0x1000; i++)
		putw2(CACHETAGS+(i<<4), 0);
	/*
	 * Turn cache on
	 */
	putb2(ENAB, getb2(ENAB)|ENABCACHE); /**/
}

void
kmapinit(void)
{
	KMap *k;
	int i;

print("low pmeg %d\n", kmapalloc.lowpmeg);
	kmapalloc.free = 0;
	k = kmapalloc.arena;
	for(i=0; i<(IOEND-IOSEGM)/BY2PG; i++,k++){
		k->va = IOSEGM+i*BY2PG;
		kunmap(k);
	}
}

KMap*
kmap1(Page *pg, ulong flag)
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
	/*
	 * Cache is virtual and a pain to deal with.
	 * Must avoid having the same entry in the cache twice, so
	 * must use NOCACHE or else extreme cleverness elsewhere.
	 */
	putpmeg(k->va, PPN(k->pa)|PTEVALID|PTEKERNEL|PTEWRITE|PTENOCACHE|flag);
	return k;
}

KMap*
kmap(Page *pg)
{
	return kmap1(pg, PTEMAINMEM);
}

void
kunmap(KMap *k)
{
	ulong pte;
	int i;

	k->pa = 0;
	lock(&kmapalloc);
	k->next = kmapalloc.free;
	kmapalloc.free = k;
	putpmeg(k->va, INVALIDPTE);
	unlock(&kmapalloc);
}

void
invalidateu(void)
{
	putpmeg(USERADDR, INVALIDPTE);
}
