#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"

/* Predeclaration */
void	pageout(Proc *p, Segment*);
int	pagepte(int, Segment*, Page**);
int	needpages(void*);
void	pager(void*);
void	executeio(void);
int	canflush(Proc *p, Segment*);

enum
{
	Maxpages = 500,		/* Max number of pageouts per segment pass */
};

Image 	swapimage;
static 	int swopen;
Page	*iolist[Maxpages];
int	ioptr;

void
swapinit(void)
{
	swapalloc.swmap = ialloc(conf.nswap, 0);
	swapalloc.top = &swapalloc.swmap[conf.nswap];
	swapalloc.alloc = swapalloc.swmap;

	swapalloc.free = conf.nswap;
}

ulong
newswap(void)
{
	char *look;
	int n;

	lock(&swapalloc);
	if(swapalloc.free == 0)
		panic("out of swap space");

	n = swapalloc.top - swapalloc.alloc;
	look = swapalloc.alloc;
	while(n && *look) {
		n--;
		look++;
	}
	if(n == 0) {
		look = swapalloc.swmap;
		while(*look)
			look++;
	}
	if(look == swapalloc.top)
		swapalloc.alloc = swapalloc.swmap;
	else
		swapalloc.alloc = look+1;

	*look = 1;
	unlock(&swapalloc);
	return (look-swapalloc.swmap) * BY2PG; 
}

void
putswap(Page *p)
{
	lock(&swapalloc);
	if(--swapalloc.swmap[((ulong)p)/BY2PG] == 0)
		swapalloc.free++;
	unlock(&swapalloc);
}

void
dupswap(Page *p)
{
	lock(&swapalloc);
	swapalloc.swmap[((ulong)p)/BY2PG]++;
	unlock(&swapalloc);
}

void
kickpager(void)
{
	static int started;

	if(started)
		wakeup(&swapalloc.r);
	else {
		kproc("pager", pager, 0);
		started = 1;
	}
}

void
pager(void *junk)
{
	Proc *p, *ep;
	Segment *s;
	int i;

	p = proctab(0);
	ep = &p[conf.nproc];
	for(;;) {
		if(waserror()) 
			panic("pager: os error\n");

		for(p = proctab(0); p < ep; p++) {
			if(p->state == Dead || p->kp)
				continue;

			sleep(&swapalloc.r, needpages, 0);

			if(swapimage.c) {
				for(i = 0; i < NSEG; i++)
					if(s = p->seg[i]) {
						pageout(p, s);
						executeio();
					}
			}
			else {
				/* Emulate the old system if no swap channel */
				print("no physical memory\n");
				tsleep(&swapalloc.r, return0, 0, 1000);
				wakeup(&palloc.r);
			}
		}

		poperror();
	}
}

void			
pageout(Proc *p, Segment *s)
{
	Pte **sm, **endsm;
	Page **pg, **epg;
	int type;

	if(!canqlock(&s->lk))	/* We cannot afford to wait, we will surely deadlock */
		return;

	if(!canflush(p, s) || s->steal) {
		qunlock(&s->lk);
		putseg(s);
		return;
	}

	if(waserror()) {
		qunlock(&s->lk);
		putseg(s);
		return;
	}

	/* Pass through the pte tables looking for memory pages to swap out */
	type = s->type&SG_TYPE;
	endsm = &s->map[SEGMAPSIZE];
	for(sm = s->map; sm < endsm && ioptr < Maxpages; sm++)
		if(*sm) {
			pg = (*sm)->pages;
			for(epg = &pg[PTEPERTAB]; pg < epg && ioptr < Maxpages; pg++)
				if(!pagedout(*pg)) {
					if((*pg)->modref & PG_REF)
						(*pg)->modref &= ~PG_REF;
					else 
					if(pagepte(type, s, pg) == 0)
						break;
				}
		}

	poperror();
	qunlock(&s->lk);
	putseg(s);

	wakeup(&palloc.r);
}

int
canflush(Proc *p, Segment *s)
{
	Proc *ep;
	int i;

	lock(s);
	if(s->ref == 1) {		/* Easy if we are the only user */
		s->ref++;
		unlock(s);
		return canpage(p);
	}
	s->ref++;
	unlock(s);

	/* Now we must do hardwork to ensure all processes which have tlb
	 * entries for this segment will be flushed if we suceed in pageing it out
	 */
	p = proctab(0);
	ep = &p[conf.nproc];
	for(; p < ep; p++)
		if(p->state != Dead)
			for(i = 0; i < NSEG; i++)
				if(p->seg[i] == s)
					if(!canpage(p))
						return 0;
	return 1;						
}

int
pagepte(int type, Segment *s, Page **pg)
{
	ulong daddr;
	char *kaddr;
	int n;
	Chan *c;
	Page *outp;
	KMap *k;

	outp = *pg;

	switch(type) {
	case SG_TEXT:					/* Revert to demand load */
		putpage(outp);
		*pg = 0;
		break;
	case SG_DATA:
		/* Unmodified data may be reverted to a demand load record if it
		 * is not the last page in the DSEG
		 */
/*							BUG: needs to check the last page
		if((outp->modref&PG_MOD) == 0) {
			putpage(outp);
			*pg = 0;
			break;
		}
*/
							/* NO break */	
	case SG_BSS:
	case SG_STACK:
	case SG_SHARED:
		lockpage(outp);
		outp->ref++;
		uncachepage(outp);
		unlockpage(outp);

		daddr = newswap();
		outp->daddr = daddr;

		/* Enter swap page into cache before segment is unlocked so that
		 * a fault will cause a cache recovery rather than a pagein on a
		 * partially written block.
		 */
		cachepage(outp, &swapimage);
		*pg = (Page*)(daddr|PG_ONSWAP);

		/* Add me to IO transaction list */
		iolist[ioptr++] = outp;
	}

	return 1;
}

void
executeio(void)
{
	Page *out;
	int i, n;
	Chan *c;
	char *kaddr;
	KMap *k;

	c = swapimage.c;

	for(i = 0; i < ioptr; i++) {
		out = iolist[i];
		if(out->ref > 2) {
			lockpage(out);
			if(out->ref > 2) {		/* Page was reclaimed, abort io */
				out->ref -= 2;
				unlockpage(out);
				continue;
			}
			unlockpage(out);
		}
		k = kmap(out);
		kaddr = (char*)VA(k);
		qlock(&c->wrl);

		/* BUG: what to do ? Nobody to tell, nowhere to go: open to suggestions 
		 *	the problem is I do not know whose page this is.
		 */
		if(waserror())
			panic("executeio: page out I/O error");

		n = (*devtab[c->type].write)(c, kaddr, BY2PG, out->daddr);
		if(n != BY2PG)
			nexterror();

		qunlock(&c->wrl);
		kunmap(k);
		poperror();

		/* Free up the page after I/O */
		lockpage(out);
		out->ref--;
		unlockpage(out);
		putpage(out);
	}

	ioptr = 0;
}

int
needpages(void *p)
{
	return palloc.freecount < HIGHWATER+MAXHEADROOM;
}

void
setswapchan(Chan *c)
{
	if(swapimage.c) {
		if(swapalloc.free != conf.nswap)
			errors("swap channel busy");
		close(swapimage.c);
	}
	incref(c);
	swapimage.c = c;
}

