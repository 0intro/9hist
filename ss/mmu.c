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

#define	NKLUDGE	8

/*
 * On SPARC, tlbpid i == context i-1 so that 0 means unallocated
 */

int	newpid(Proc*);
void	purgepid(int);
int	pidtime[NTLBPID];	/* should be per m */

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
		panic("mapstack %s %d %lux 0x%lux 0x%lux", p->text, p->pid, p->upage, p->upage->pa, p->upage->va);
	tlbphys = PPN(p->upage->pa)|PTEVALID|PTEWRITE|PTEKERNEL|PTEMAINMEM;
	putcontext(tp-1);
	/*
	 * shouldn't need putpmeg because nothing has been mapped at
	 * USERADDR in this context except this page.  however, it crashes.
	 */
	putpmeg(USERADDR, tlbphys);
	u = (User*)USERADDR;
}

/*
 * Process must be non-interruptible
 */
int
newpid(Proc *p)
{
	int i, j, k;
	ulong t;
	Proc *sp;

	t = ~0;
	i = 1+((m->ticks)&(NCONTEXT-1));	/* random guess */
	for(j=1; j<NTLBPID; j++)
		if(pidtime[j] < t){
			i = j;
			t = pidtime[j];
		}
	
	sp = m->pidproc[i];
	if(sp){
		sp->pidonmach[m->machno] = 0;
		purgepid(i);
	}
	pidtime[i] = m->ticks;
	m->pidproc[i] = p;
	m->lastpid = i;
	putcontext(i-1);
	/*
	 * kludge: each context is allowed NKLUDGE pmegs, NKLUDGE-1 for text & data and 1 for stack
	 */
	for(j=0; j<NKLUDGE-1; j++)
		putsegm(UZERO+j*BY2SEGM, kmapalloc.lowpmeg+(NKLUDGE*(i-1))+j);
	putsegm(TSTKTOP-BY2SEGM, kmapalloc.lowpmeg+(NKLUDGE*(i-1))+(NKLUDGE-1));
	for(j=0; j<PG2SEGM; j++){
		for(k=0; k<NKLUDGE-1; k++)
			putpmeg(UZERO+k*BY2SEGM+j*BY2PG, INVALIDPTE);
		putpmeg((TSTKTOP-BY2SEGM)+j*BY2PG, INVALIDPTE);
	}
	return i;
}

void
putcontext(int c)
{
	m->pidhere[c+1] = 1;
	putcxreg(c);
}

void
flushcontext(void)
{
	int i;

	/*
	 * Clear context from cache
	 */
	for(i=0; i<0x1000; i+=16)
		putwE16((i<<4), 0);
}

void
purgepid(int pid)
{
	if(m->pidhere[pid] == 0)
		return;
	memset(m->pidhere, 0, sizeof m->pidhere);
	putcontext(pid-1);
	flushcontext();
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

		for(l=UZERO; l<(KZERO&VAMASK); l+=BY2SEGM)
			putsegm(l, INVALIDPMEG);

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
	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newpid(p);
		p->pidonmach[m->machno] = tp;
	}
	/*
	 * kludge part 2: make sure we've got a valid segment
	 */
	if(tlbvirt>=TSTKTOP || (UZERO+(NKLUDGE-1)*BY2SEGM<=tlbvirt && tlbvirt<(TSTKTOP-BY2SEGM))){
		pprint("putmmu %lux", tlbvirt);
		pexit("Suicide", 0);
	}
	putpmeg(tlbvirt, tlbphys);
	spllo();
}

void
putpmeg(ulong virt, ulong phys)
{
	int i;
	int tp;

	virt &= VAMASK;
	virt &= ~(BY2PG-1);
	/*
	 * Flush old entries from cache
	 */
	for(i=0; i<0x100; i+=16)
		putwD16(virt+(i<<4), 0);
	if(u && u->p)
		m->pidhere[u->p->pidonmach[m->machno]] = 1;	/* UGH! */
	putw4(virt, phys);
}

void
flushmmu(void)
{
	int tp;

	splhi();
	flushcontext();
	tp = u->p->pidonmach[m->machno];
	if(tp)
		pidtime[tp] = 0;
	/* easiest is to forget what pid we had.... */
	memset(u->p->pidonmach, 0, sizeof u->p->pidonmach);
	/* ....then get a new one by trying to map our stack */
	mapstack(u->p);
	spllo();
}

void
cacheinit(void)
{
	int c, i;

	/*
	 * Initialize cache by clearing the valid bit
	 * (along with the others) in all cache entries
	 */
	for(c=0; c<NCONTEXT; c++){	/* necessary? */
		putcontext(c);
		for(i=0; i<0x1000; i++)
			putw2(CACHETAGS+(i<<4), 0);
	}
	putcontext(0);

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

print("low pmeg %d high pmeg %d\n", kmapalloc.lowpmeg, TOPPMEG);
	kmapalloc.free = 0;
	k = kmapalloc.arena;
	for(i=0; i<(IOEND-IOSEGM)/BY2PG; i++,k++){
		k->va = IOSEGM+i*BY2PG;
		kunmap(k);
	}
}

KMap*
kmappa(ulong pa, ulong flag)
{
	KMap *k;
	ulong s;

	lock(&kmapalloc);
	k = kmapalloc.free;
	if(k == 0){
		dumpstack();
		panic("kmap");
	}
	kmapalloc.free = k->next;
	unlock(&kmapalloc);
	k->pa = pa;
	/*
	 * Cache is virtual and a pain to deal with.
	 * Must avoid having the same entry in the cache twice, so
	 * must use NOCACHE or else extreme cleverness elsewhere.
	 */
	s = splhi();
#ifdef stupid
{
	int i, c, d;

	c = u->p->pidonmach[m->machno];
	/*
	 * Flush old entries from cache
	 */
	for(d=0; d<NCONTEXT; d++){
		putcontext(d);
		for(i=0; i<0x100; i+=16)
			putwD16(k->va+(i<<4), 0);
	}
	putcontext(c-1);
	if(u && u->p)
		m->pidhere[c] = 1;	/* UGH! */
	putw4(k->va, PPN(pa)|PTEVALID|PTEKERNEL|PTEWRITE|PTENOCACHE|flag);
}
#else
	putpmeg(k->va, PPN(pa)|PTEVALID|PTEKERNEL|PTEWRITE|PTENOCACHE|flag);
#endif
	splx(s);
	return k;
}

KMap*
kmap(Page *pg)
{
	return kmappa(pg->pa, PTEMAINMEM);
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
