#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

void	compile(void);
#define	NCODE	1024
static	ulong	code[NCODE];
static	ulong	*codep = code;

void	(*putcontext)(ulong);
void	(*putenab)(ulong);
ulong	(*getenab)(void);
void	(*putpmegspace)(ulong, ulong);
void	(*putsysspace)(ulong, ulong);
ulong	(*getsysspace)(ulong);
ulong	(*flushcx)(ulong);
ulong	(*flushpg)(ulong);

struct
{
	Lock;
	int	lowpmeg;
	KMap	*free;
	KMap	arena[(IOEND-IOSEGM0)/BY2PG];
}kmapalloc;

int	NKLUDGE;

/*
 * On SPARC, tlbpid i == context i-1 so that 0 means unallocated
 */

int	newpid(Proc*);
void	purgepid(int);
void	flushcontext(void);
void	putpmegnf(ulong, ulong);

int	pidtime[NTLBPID];	/* should be per m */

/*
 * Called splhi, not in Running state
 */
void
mapstack(Proc *p)
{
	short tp;
	ulong tlbphys;

	if(p->newtlb) {
		mmurelease(p);
		p->newtlb = 0;
	}

	tp = p->pidonmach[m->machno];
	if(tp == 0){
		tp = newpid(p);
		p->pidonmach[m->machno] = tp;
	}
	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)) && p->pid != 0)
		panic("mapstack %s %d %lux 0x%lux 0x%lux", p->text, p->pid, p->upage, p->upage->pa, p->upage->va);
	tlbphys = PPN(p->upage->pa)|PTEVALID|PTEWRITE|PTEKERNEL|PTEMAINMEM;
	putcontext(tp-1);
	/*
	 * Don't need to flush cache because no other page has been
	 * mapped at USERADDR in this context; can call putpmegnf.
	 */
	putpmegnf(USERADDR, tlbphys);
	u = (User*)USERADDR;
}

void
mmurelease(Proc *p)
{
	int tp;

	tp = p->pidonmach[m->machno];
	if(tp)
		pidtime[tp] = 0;
	/* easiest is to forget what pid we had.... */
	memset(p->pidonmach, 0, sizeof p->pidonmach);
}

/*
 * Process must be non-interruptible
 */
int
newpid(Proc *p)
{
	int i, j, nc;
	ulong t;
	Proc *sp;

	t = ~0;
	nc = conf.ncontext;
	i = 1+((m->ticks)&(nc-1));	/* random guess */
	nc++;
	for(j=1; t && j<nc; j++)
		if(pidtime[j] < t){
			i = j;
			t = pidtime[j];
		}
	
	sp = m->pidproc[i];
	if(sp)
		sp->pidonmach[m->machno] = 0;
	purgepid(i);	/* also does putcontext */
	pidtime[i] = m->ticks;
	m->pidproc[i] = p;
	m->lastpid = i;

	/*
	 * kludge: each context is allowed NKLUDGE pmegs.
	 * NKLUDGE-1 for text & data and 1 for stack.
	 * initialize by giving just a stack segment.
	 */
	i--;	/* now i==context */
	p->nmmuseg = 0;
	for(j=0; j<NKLUDGE-1; j++)
		putsegm(UZERO+j*BY2SEGM, INVALIDPMEG);
	putsegm(TSTKTOP-BY2SEGM, kmapalloc.lowpmeg+NKLUDGE*i+(NKLUDGE-1));
	for(j=0; j<PG2SEGM; j++)
		putpmegnf((TSTKTOP-BY2SEGM)+j*BY2PG, INVALIDPTE);
	return i+1;
}

void
flushcontext(void)
{
	ulong a;

	a = 0;
	do
		a = flushcx(a);
	while(a < conf.vacsize);
}

void
purgepid(int pid)
{
	putcontext(pid-1);
	flushcontext();
}

void
mmuinit(void)
{
	ulong ktop, l, i, j, c, pte;

	compile();
	/*
	 * xinit sets conf.npage0 to maximum kernel address
	 */
	ktop = PADDR(conf.npage0);
	/*
	 * First map lots of memory as kernel addressable in all contexts
	 */
	i = 0;		/* used */
	for(c=0; c<conf.ncontext; c++)
		for(i=0; i < ktop/BY2SEGM; i++)
			putcxsegm(c, KZERO+i*BY2SEGM, i);

	kmapalloc.lowpmeg = i;
	if(PADDR(ktop) & (BY2SEGM-1))
		kmapalloc.lowpmeg++;

	/*
	 * Make sure cache is turned on for kernel
	 */
	pte = PTEVALID|PTEWRITE|PTEKERNEL|PTEMAINMEM;
	ktop /= BY2PG;
	for(i=0; i < ktop; i++)
		putpmeg(KZERO+i*BY2PG, pte+i);

	/*
	 * Create invalid pmeg; use highest segment
	 */
	putsegm(INVALIDSEGM, INVALIDPMEG);
	for(i=0; i<PG2SEGM; i++)
		putpmeg(INVALIDSEGM+i*BY2PG, INVALIDPTE);

	for(c=0; c<conf.ncontext; c++){
		putcontext(c);
		putsegm(INVALIDSEGM, INVALIDPMEG);
		/*
		 * Invalidate user addresses
		 */
		for(l=UZERO; l<(KZERO&VAMASK); l+=BY2SEGM)
			putsegm(l, INVALIDPMEG);

		/*
		 * Map ROM
		 */
		putsegm(ROMSEGM, ROMPMEG);
		if(c == 0){
			pte = PTEVALID|PTEKERNEL|PTENOCACHE|
				PTEIO|((EPROM>>PGSHIFT)&0xFFFF);
			for(i=0; i<PG2ROM; i++)
				putpmeg(IOSEGM0+i*BY2PG, pte+i);
			for(; i<PG2SEGM; i++)
				putpmeg(IOSEGM0+i*BY2PG, INVALIDPTE);
		}
		/*
		 * Segments for IO and kmap
		 */
		for(j=0; j<NIOSEGM; j++){
			putsegm(IOSEGM0+j*BY2SEGM, IOPMEG0+j);
			if(c == 0)
				for(i=0; i<PG2SEGM; i++)
					putpmeg(IOSEGM0+j*BY2SEGM+i*BY2PG, INVALIDPTE);
		}
	}
	putcontext(0);
	NKLUDGE = ((TOPPMEG-kmapalloc.lowpmeg)/conf.ncontext);
if(NKLUDGE>11)NKLUDGE=11;
}

void
putmmu(ulong tlbvirt, ulong tlbphys, Page *pg)
{
	short tp;
	Proc *p;
	ulong seg, l;
	int j, k;

	USED(pg);
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
	if(TSTKTOP-BY2SEGM<=tlbvirt && tlbvirt<TSTKTOP)	/* stack; easy */
		goto put;
	/* UZERO is known to be zero here */
	if(tlbvirt < UZERO+p->nmmuseg*BY2SEGM)		/* in range; easy */
		goto put;
	seg = tlbvirt/BY2SEGM;
	if(seg >= (UZERO/BY2SEGM)+(NKLUDGE-1)){
		pprint("putmmu %lux\n", tlbvirt);
print("putmmu %lux %d %s\n", tlbvirt, seg, p->text);
		pexit("Suicide", 1);
	}
	/*
	 * Prepare mmu up to this address
	 */
	tp = (tp-1)*NKLUDGE;	/* now tp==base of pmeg area for this proc */
	l = UZERO+p->nmmuseg*BY2SEGM;
	for(j=p->nmmuseg; j<=seg; j++){
		putsegm(l, kmapalloc.lowpmeg+tp+j);
		for(k=0; k<PG2SEGM; k++,l+=BY2PG)
			putpmegnf(l, INVALIDPTE);
	}
	p->nmmuseg = seg+1;
    put:
	putpmeg(tlbvirt, tlbphys);
	spllo();
}

void
putpmeg(ulong virt, ulong phys)
{
	ulong a, evirt;

	virt &= VAMASK;
	virt &= ~(BY2PG-1);
	/*
	 * Flush old entries from cache
	 */
	a = virt;
	evirt = virt+BY2PG;
	do
		a = flushpg(a);
	while(a < evirt);
	putpmegspace(virt, phys);
}

void
putpmegnf(ulong virt, ulong phys)	/* no need to flush */
{
	virt &= VAMASK;
	virt &= ~(BY2PG-1);
	putpmegspace(virt, phys);
}

void
flushmmu(void)
{
	splhi();
	u->p->newtlb = 1;
	mapstack(u->p);
	spllo();
}

void
cacheinit(void)
{
	int i;

	putcontext(0);
	/*
	 * Initialize cache by clearing the valid bit
	 * (along with the others) in all cache entries
	 */
	for(i=0; i<conf.vacsize; i+=conf.vaclinesize)
		putsysspace(CACHETAGS+i, 0);

	/*
	 * Turn cache on
	 */
	putenab(getenab()|ENABCACHE); /**/
}

void
kmapinit(void)
{
	KMap *k;
	int i;

	kmapalloc.free = 0;
	k = kmapalloc.arena;
	for(i=0; i<(IOEND-IOSEGM0)/BY2PG; i++,k++){
		k->va = IOSEGM0+i*BY2PG;
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
	s = splhi();
	putpmeg(k->va, PPN(pa)|PTEVALID|PTEKERNEL|PTEWRITE|flag);
	splx(s);
	return k;
}

ulong
kmapregion(ulong pa, ulong n, ulong flag)
{
	KMap *k;
	ulong va;
	int i, j;

	/*
	 * kmap's are initially in reverse order so rearrange them.
	 */
	i = (n+(BY2PG-1))/BY2PG;
	va = 0;
	for(j=i-1; j>=0; j--){
		k = kmappa(pa+j*BY2PG, flag);
		if(va && va != k->va+BY2PG)
			systemreset();
		va = k->va;
	}
	return va;
}

KMap*
kmap(Page *pg)
{
	/*
	 * Cache is virtual and a pain to deal with.
	 * Must avoid having the same entry in the cache twice, so
	 * must use NOCACHE or else extreme cleverness elsewhere.
	 */
	return kmappa(pg->pa, PTEMAINMEM|PTENOCACHE);
}

KMap*
kmapperm(Page *pg)
{
	/*
	 * Here we know it's a permanent entry and can be cached.
	 */
	return kmappa(pg->pa, PTEMAINMEM);
}

void
kunmap(KMap *k)
{
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

/*
 * Compile MMU code for this machine, since the MMU can only
 * be addressed from parameterless machine instructions.
 * What's wrong with MMUs you can talk to from C?
 */

/* op3 */
#define	LD	0
#define	ADD	0
#define	OR	2
#define	LDA	16
#define	LDUBA	17
#define	STA	20
#define	STBA	21
#define	JMPL	56
/* op2 */
#define	SETHI	4

void	*compileconst(int, ulong, int);	/* value to/from constant address */
void	*compileldaddr(int, int);	/* value from parameter address */
void	*compilestaddr(int, int);	/* value to parameter address */
void	*compile16(ulong, int);		/* 16 stores of zero */
void	*compile1(ulong, int);		/* 1 stores of zero */

#define	ret()	{*codep++ = (2<<30)|(0<<25)|(JMPL<<19)|(15<<14)|(1<<13)|8;}
#define	nop()	{*codep++ = (0<<30)|(0<<25)|(SETHI<<22)|(0>>10);}

void
compile(void)
{
	putcontext = compileconst(STBA, CONTEXT, 2);
	getenab = compileconst(LDUBA, ENAB, 2);
	putenab = compileconst(STBA, ENAB, 2);
	putpmegspace = compilestaddr(STA, 4);
	putsysspace = compilestaddr(STA, 2);
	getsysspace = compileldaddr(LDA, 2);
	if(conf.ss2){
		flushpg = compile1(BY2PG, 6);
		flushcx = compile16(conf.vaclinesize, 7);
	}else{
		flushpg = compile16(conf.vaclinesize, 0xD);
		flushcx = compile16(conf.vaclinesize, 0xE);
	}
}

void
parameter(int param, int reg)
{
	param += 1;	/* 0th parameter is 1st word on stack */
	param *= 4;
	/* LD #param(R1), Rreg */
	*codep++ = (3<<30)|(reg<<25)|(LD<<19)|(1<<14)|(1<<13)|param;
}

void
constant(ulong c, int reg)
{
	*codep++ = (0<<30)|(reg<<25)|(SETHI<<22)|(c>>10);
	if(c & 0x3FF)
		*codep++ = (2<<30)|(reg<<25)|(OR<<19)|(reg<<14)|(1<<13)|(c&0x3FF);
}

/*
 * void f(int c) { *(word*,asi)addr = c } for stores
 * ulong f(void)  { return *(word*,asi)addr } for loads
 */
void*
compileconst(int op3, ulong addr, int asi)
{
	void *a;

	a = codep;
	constant(addr, 8);	/* MOVW $CONSTANT, R8 */
	ret();			/* JMPL 8(R15), R0 */
	/* in delay slot 	   st or ld R7, (R8+R0, asi)	*/
	*codep++ = (3<<30)|(7<<25)|(op3<<19)|(8<<14)|(asi<<5);
	return a;
}

/*
 * ulong f(ulong addr)  { return *(word*,asi)addr }
 */
void*
compileldaddr(int op3, int asi)
{
	void *a;

	a = codep;
	ret();			/* JMPL 8(R15), R0 */
	/* in delay slot 	   ld (R7+R0, asi), R7	*/
	*codep++ = (3<<30)|(7<<25)|(op3<<19)|(7<<14)|(asi<<5);
	return a;
}

/*
 * void f(ulong addr, int c) { *(word*,asi)addr = c }
 */
void*
compilestaddr(int op3, int asi)
{
	void *a;

	a = codep;
	parameter(1, 8);	/* MOVW (4*1)(FP), R8 */
	ret();			/* JMPL 8(R15), R0 */
	/* in delay slot 	   st R8, (R7+R0, asi)	*/
	*codep++ = (3<<30)|(8<<25)|(op3<<19)|(7<<14)|(asi<<5);
	return a;
}

/*
 * ulong f(ulong addr) { *addr=0; addr+=offset; return addr}
 * offset can be anything
 */
void*
compile1(ulong offset, int asi)
{
	void *a;

	a = codep;
	/* ST R0, (R7+R0, asi)	*/
	*codep++ = (3<<30)|(0<<25)|(STA<<19)|(7<<14)|(asi<<5);
	if(offset < (1<<12)){
		ret();			/* JMPL 8(R15), R0 */
		/* in delay slot ADD $offset, R7 */
		*codep++ = (2<<30)|(7<<25)|(ADD<<19)|(7<<14)|(1<<13)|offset;
	}else{
		constant(offset, 8);
		ret();			/* JMPL 8(R15), R0 */
		/* in delay slot ADD R8, R7 */
		*codep++ = (2<<30)|(7<<25)|(ADD<<19)|(7<<14)|(0<<13)|8;
	}
	return a;
}

/*
 * ulong f(ulong addr) { for(i=0;i<16;i++) {*addr=0; addr+=offset}; return addr}
 * offset must be less than 1<<12
 */
void*
compile16(ulong offset, int asi)
{
	void *a;
	int i;

	a = codep;
	for(i=0; i<16; i++){
		/* ST R0, (R7+R0, asi)	*/
		*codep++ = (3<<30)|(0<<25)|(STA<<19)|(7<<14)|(asi<<5);
		/* ADD $offset, R7 */
		*codep++ = (2<<30)|(7<<25)|(ADD<<19)|(7<<14)|(1<<13)|offset;
	}
	ret();			/* JMPL 8(R15), R0 */
	nop();
	return a;
}
