/* compile-time features
   IALLOC use all blocks given to ifree, otherwise ignore unordered blocks
   MSTATS enable statistics 
   debug enable assertion checking
   longdebug full arena checks at every transaction
*/
#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

#define INT int
#define ALIGN int
#define NALIGN 1
#define WORD sizeof(union store)
#define BLOCK 4096
#define BUSY 1
#define NULL 0
#define testbusy(p) ((INT)(p)&BUSY)
#define setbusy(p) (union store *)((INT)(p)|BUSY)
#define clearbusy(p) (union store *)((INT)(p)&~BUSY)
#define IALLOC

typedef
union store
{
	union store	*ptr;
	ALIGN	dummy[NALIGN];
	int	calloc;		/*calloc clears an array of integers*/
} Store;

static	int	draincache(void);
static	void*	stdmalloc(long);
static	int	stdfree(Store*);
static	void	ifree(char*, long);

#ifdef longdebug
#define debug 1
#endif
#ifdef debug
#define ASSERT(p) if(!(p))botch("p");else
static
botch(char *s)
{
	char *c;

	c = "assertion botched: ";
	write(2, c, strlen(c));
	write(2, s, strlen(s));
	write(2, "\n", 1);
	abort();
}
static	int	allock(Store*);
#else
#define ASSERT(p)
#endif

/*	C storage allocator
 *	circular first-fit strategy
 *	works with noncontiguous, but monotonically linked, arena
 *	each block is preceded by a ptr to the (pointer of) 
 *	the next following block
 *	blocks are exact number of words long 
 *	aligned to the data type requirements of ALIGN
 *	pointers to blocks must have BUSY bit 0
 *	bit in ptr is 1 for busy, 0 for idle
 *	gaps in arena are merely noted as busy blocks
 *	last block of arena is empty and
 *	has a pointer to first
 *	idle blocks are coalesced during space search
 *
 *	a different implementation may need to redefine
 *	ALIGN, NALIGN, BLOCK, BUSY, INT
 *	where INT is integer type to which a pointer can be cast
 */


/* alloca should have type union store.
 * The funny business gets it initialized without complaint
 */
#define addr(a) (Store*)&a
static	char *alloca;
static	char *alloca = (char*)&alloca + BUSY;	/* initial arena */
static	Store *allocs = addr(alloca);	/*arena base*/
static	Store *allocc = addr(alloca);	/*all prev blocks known busy*/
static	Store *allocp = addr(alloca);	/*search ptr*/
static	Store *alloct = addr(alloca);	/*top cell*/
static	Store *allocx;	/*for benefit of realloc*/

/* a cache list of frequently-used sizes is maintained. From each
 * cache entry hangs a chain of available blocks 
 * malloc(0) shuts off caching (to keep freed data clean)
 */

#define CACHEMAX 256	/* largest block to be cached (in words) */
#define CACHESIZ  53	/* number of entries (prime) */

static Store *cache[CACHESIZ];
static int cachemax = CACHEMAX;

#ifdef	MSTATS
#define	Mstats(e) e
static	long	nmalloc, nrealloc, nfree;	/* call statistics */
static	long	walloc, wfree;			/* space statistics */
static	long	chit, ccoll, cdrain, cavail;	/* cache statistics */
#else
#define Mstats(e)
#endif

static QLock mlock;

void*
malloc(ulong nbytes)
{
	Store *p;
	long nw;
	Store **cp;
	void *mem;

	nw = (nbytes+WORD+WORD-1)/WORD;
	qlock(&mlock);
	Mstats((nmalloc++, walloc += nw));
	if(nw<cachemax) { 
		if(nw >= 2) {
			cp = &cache[nw%CACHESIZ];
			p = *cp;
			if(p && nw == clearbusy(p->ptr)-p) {
				ASSERT(testbusy(p->ptr));
				*cp = (++p)->ptr;
				Mstats((chit++, cavail--));
				qunlock(&mlock);
				return (char*)p;
			}
		} else {
			draincache();
			cachemax = 0;
		}
	}
	p = stdmalloc(nw);
	qunlock(&mlock);
	return p;
}

static void*
stdmalloc(long nw)
{
	Store *p, *q;
	int temp;
	Page *page;

	ASSERT(allock(allocp));
	for(;;) {	/* done at most thrice */
		p = allocp;
		for(temp=0; ; ) {
			if(!testbusy(p->ptr)) {
				allocp = p;
				while(!testbusy((q=p->ptr)->ptr)) {
					ASSERT(q>p);
					p->ptr = q->ptr;
				}
				if(q>=p+nw && p+nw>=p)
					goto found;
			}
			q = p;
			p = clearbusy(p->ptr);
			if(p <= q) {
				ASSERT(p == allocs && q == alloct);
				if(++temp>1)
					break;
				ASSERT(allock(allocc));
				p = allocc;
			}
		}

		/* No memory in the free list.  Call newpage and kmap to get
		 * some more, and ifree to put it in the list.
		 */
		page = newpage(1, 0, 0);
		page->va = VA(kmap(page));
		draincache();
		ifree((char *) page->va, 1 << PGSHIFT);
	}
found:
	allocp += nw;
	if(q>allocp) {
		allocx = allocp->ptr;
		allocp->ptr = p->ptr;
	}
	p->ptr = setbusy(allocp);
	if(p<=allocc) {
		ASSERT(p==allocc);
		while(testbusy(allocc->ptr)
		     && (q=clearbusy(allocc->ptr))>allocc)
			allocc = q;
	}
	return(p+1);
}

void
free(void *ap)
{
	Store *p = ap, *q;
	long nw;
	Store **cp;

	if(p==NULL)
		return;
	--p;
	qlock(&mlock);
	ASSERT(allock(p));
	ASSERT(testbusy(p->ptr));
	ASSERT(!cached(p));
	nw = clearbusy(p->ptr) - p;
	Mstats((nfree++, wfree += nw));
	ASSERT(nw>0);
	if(nw<cachemax && nw>=2) {
		cp = &cache[nw%CACHESIZ];
		q = *cp;
		if(!q || nw==clearbusy(q->ptr)-q) {
			p[1].ptr = q;
			*cp = p;
			Mstats(cavail++);
			qunlock(&mlock);
			return;
		} else Mstats(q && ccoll++);
	}
	stdfree(p+1);
	qunlock(&mlock);
}

/*	freeing strategy tuned for LIFO allocation
*/
static
stdfree(Store *p)
{
	allocp = --p;
	if(p < allocc)
		allocc = p;
	ASSERT(allock(allocp));
	p->ptr = clearbusy(p->ptr);
	ASSERT(p->ptr > allocp);
}

static
draincache(void)
{
	Store **cp = cache+CACHESIZ;
	Store *q;
	int anyfreed = 0;

	while(--cp>=cache) {
		while(q = *cp) {
			ASSERT(testbusy(q->ptr));
			ASSERT((clearbusy(q->ptr)-q)%CACHESIZ==cp-cache);
			ASSERT(q>=allocs&&q<=alloct);
			stdfree(++q);
			anyfreed++;
			*cp = q->ptr;
		}
	}
	Mstats((cdrain+=anyfreed, cavail=0));
	return anyfreed;
}

/* ifree(q, nbytes) inserts a block that did not come
 * from malloc into the arena
 *
 * q points to new block
 * r points to last of new block
 * p points to last cell of arena before new block
 * s points to first cell of arena after new block
*/

static
void
ifree(char *qq, long nbytes)
{
	Store *p, *q, *r, *s;

	q = (Store *)qq;
	r = q + (nbytes/WORD) - 1;
	q->ptr = r;
	if(q > alloct) {
		p = alloct;
		s = allocs;
		alloct = r;
	} else {
#ifdef IALLOC
		/* useful only in small address spaces */
		for(p=allocs; ; p=s) {
			s = clearbusy(p->ptr);
			if(s==allocs)
				break;
			ASSERT(s>p);
			if(s>r) {
				if(p<q)
					break;
				else
					ASSERT(p>r);
			}
		}
		if(allocs > q)
			allocs = q;
		if(allocc > q)
			allocc = q;
		allocp = allocc;
#else
		return;
#endif
	}
	p->ptr = q==p+1? q: setbusy(q);
	r->ptr = s==r+1? s: setbusy(s);
	while(testbusy(allocc->ptr))
		allocc = clearbusy(allocc->ptr);
}

/*	realloc(p, nbytes) reallocates a block obtained from malloc()
 *	and freed since last call of malloc()
 *	to have new size nbytes, and old content
 *	returns new location, or 0 on failure
*/

void*
realloc(void *pp, ulong nbytes)
{
	Store *p = pp;
	Store *s, *t;
	Store *q;
	long nw, onw;

	if(p==NULL)
		return malloc(nbytes);
	qlock(&mlock);
	Mstats(nrealloc++);
	ASSERT(allock(p-1));
	if(testbusy(p[-1].ptr))
		stdfree(p);
	onw = p[-1].ptr - p;
	nw = (nbytes+WORD-1)/WORD;
	q = (Store *)stdmalloc(nw+1);
	if(q!=NULL && q!=p) {
		ASSERT(q<p||q>p[-1].ptr);
		if(nw<onw) {
			Mstats(wfree += onw-nw);
			onw = nw;
		} else Mstats(walloc += nw-onw);
		for(s=p, t=q; onw--!=0; )
			*t++ = *s++;
		ASSERT(clearbusy(q[-1].ptr)-q==nw);
		if(q<p && q+nw>=p)
			(q+(q+nw-p))->ptr = allocx;
		ASSERT(allock(q-1));
	}
	qunlock(&mlock);
	return q;
}

#ifdef debug
static
allock(Store *q)
{
#ifdef longdebug
	register Store *p, *r;
	register Store **cp;
	int x, y;
	for(cp=cache+CACHESIZ; --cp>=cache; ) {
		if((p= *cp)==0)
			continue;
		x = clearbusy(p->ptr) - p;
		ASSERT(x%CACHESIZ==cp-cache);
		for( ; p; p = p[1].ptr) {
			ASSERT(testbusy(p->ptr));
			ASSERT(clearbusy(p->ptr)-p==x);
		}
	}
	x = 0, y = 0;
	p = allocs;
	for( ; (r=clearbusy(p->ptr)) > p; p=r) {
		if(p==allocc)
			y++;
		ASSERT(y||testbusy(p->ptr));
		if(p==q)
			x++;
	}
	ASSERT(r==allocs);
	ASSERT(x==1||p==q);
	ASSERT(y||p==allocc);
	return(1);
#else
	ASSERT((unsigned)q/WORD*WORD==(unsigned)q);
	ASSERT(q>=allocs&&q<=alloct);
#endif
}
#endif

mstats(void)
{
#ifdef MSTATS
	fprint(2, "Malloc statistics, including overhead bytes\n");
	fprint(2, "Arena: bottom %ld, top %ld\n",
		(long)clearbusy(alloca), (long)alloct);
	fprint(2, "Calls: malloc %ld, realloc %ld, free %ld\n",
		nmalloc, nrealloc, nfree);
	fprint(2, "Bytes: allocated or extended %ld, ",
		walloc*WORD);
	fprint(2, "freed or cut %ld\n", wfree*WORD);
	fprint(2,"Cache: hits %ld, collisions %ld, discards %ld, avail %ld\n",
		chit, ccoll, cdrain, cavail);
#endif
}

#ifdef debug
cached(Store *p)
{
	Store *q = cache[(clearbusy(p->ptr)-p)%CACHESIZ];
	for( ; q; q=q[1].ptr)
		ASSERT(p!=q);
	return 0;
}
#endif

void *calloc(ulong n, ulong size){
	void *p;

	size = size*n;
	p = malloc(size);
	memset(p, 0, size);
	return p;
}
