#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

Page *lkpage(ulong addr);
Page *snewpage(ulong addr);
void lkpgfree(Page*);
void imagereclaim(void);

/* System specific segattach devices */
#include "segment.h"

#define IHASHSIZE	64
#define ihash(s)	imagealloc.hash[s%IHASHSIZE]
struct Imagealloc
{
	Lock;
	Image	*free;
	Image	*hash[IHASHSIZE];
}imagealloc;

struct segalloc
{
	Lock;
	Segment *free;
}segalloc;

static QLock ireclaim;

void
initseg(void)
{
	Segment *s, *se;
	Image *i, *ie;

	segalloc.free = ialloc(conf.nseg*sizeof(Segment), 0);
	imagealloc.free = ialloc(conf.nimage*sizeof(Image), 0);

	se = &segalloc.free[conf.nseg-1];
	for(s = segalloc.free; s < se; s++)
		s->next = s+1;
	s->next = 0;

	ie = &imagealloc.free[conf.nimage-1];
	for(i = imagealloc.free; i < ie; i++)
		i->next = i+1;
	i->next = 0;
}

Segment *
newseg(int type, ulong base, ulong size)
{
	Segment *s;

	if(size > (SEGMAPSIZE*PTEPERTAB))
		error(Enovmem);

	for(;;) {
		lock(&segalloc);
		if(s = segalloc.free) {
			segalloc.free = s->next;
			unlock(&segalloc);

			s->ref = 1;
			s->steal = 0;
			s->type = type;
			s->base = base;
			s->top = base+(size*BY2PG);
			s->size = size;
			s->image = 0;
			s->fstart = 0;
			s->flen = 0;
			s->pgalloc = 0;
			s->pgfree = 0;
			s->flushme = 0;
			memset(s->map, 0, sizeof(s->map));

			return s;
		}
		unlock(&segalloc);
		resrcwait("no segments");
	}
}

void
putseg(Segment *s)
{
	Pte **pp, **emap;
	Image *i;

	if(s == 0)
		return;

	i = s->image;
	if(i && i->s == s && s->ref == 1){
		lock(i);
		if(s->ref == 1)
			i->s = 0;
		unlock(i);
	}

	if(decref(s) == 0) {
		qlock(&s->lk);
		if(i)
			putimage(i);

		emap = &s->map[SEGMAPSIZE];
		for(pp = s->map; pp < emap; pp++)
			if(*pp)
				freepte(s, *pp);

		qunlock(&s->lk);

		lock(&segalloc);
		s->next = segalloc.free;		
		segalloc.free = s;
		unlock(&segalloc);
	}
}

void
relocateseg(Segment *s, ulong offset)
{
	Pte **p, **endpte;
	Page **pg, **endpages;

	endpte = &s->map[SEGMAPSIZE];
	for(p = s->map; p < endpte; p++)
		if(*p) {
			endpages = &((*p)->pages[PTEPERTAB]);
			for(pg = (*p)->pages; pg < endpages; pg++)
				if(*pg)
					(*pg)->va += offset;
		}
}

Segment*
dupseg(Segment *s)
{
	Pte *pte;
	Segment *n;
	int i;

	switch(s->type&SG_TYPE) {
	case SG_TEXT:			/* New segment shares pte set */
	case SG_SHARED:
	case SG_PHYSICAL:
	case SG_SHDATA:
		incref(s);
		return s;

	case SG_BSS:			/* Just copy on write */
	case SG_STACK:
		qlock(&s->lk);
		n = newseg(s->type, s->base, s->size);
		goto copypte;

	case SG_DATA:			/* Copy on write plus demand load info */
		qlock(&s->lk);
		n = newseg(s->type, s->base, s->size);

		incref(s->image);
		n->image = s->image;
		n->fstart = s->fstart;
		n->flen = s->flen;
	copypte:
		for(i = 0; i < SEGMAPSIZE; i++)
			if(pte = s->map[i])
				n->map[i] = ptecpy(pte);

		n->flushme = s->flushme;
		qunlock(&s->lk);
		return n;	
	}

	panic("dupseg");
}

void
segpage(Segment *s, Page *p)
{
	Pte **pte;
	ulong off;
	Page **pg;

	if(p->va < s->base || p->va >= s->top)
		panic("segpage");

	off = p->va - s->base;
	pte = &s->map[off/PTEMAPMEM];
	if(*pte == 0)
		*pte = ptealloc();

	pg = &(*pte)->pages[(off&(PTEMAPMEM-1))/BY2PG];
	*pg = p;
	if(pg < (*pte)->first)
		(*pte)->first = pg;
	if(pg > (*pte)->last)
		(*pte)->last = pg;
}

Image*
attachimage(int type, Chan *c, ulong base, ulong len)
{
	Image *i, **l;

	lock(&imagealloc);

	/*
	 * Search the image cache for remains of the text from a previous 
	 * or currently running incarnation 
	 */
	for(i = ihash(c->qid.path); i; i = i->hash) {
		if(c->qid.path == i->qid.path) {
			lock(i);
			if(eqqid(c->qid, i->qid))
			if(eqqid(c->mqid, i->mqid))
			if(c->mchan == i->mchan)
			if(c->type == i->type) {
				i->ref++;
				goto found;
			}
			unlock(i);
		}
	}
	
	/*
	 * imagereclaim dumps pages from the free list which are cached by image
	 * structures. This should free some image structures.
	 */
	while(!(i = imagealloc.free)) {
		unlock(&imagealloc);
		imagereclaim();
		resrcwait(0);
		lock(&imagealloc);
	}

	imagealloc.free = i->next;

	lock(i);
	incref(c);
	i->c = c;
	i->type = c->type;
	i->qid = c->qid;
	i->mqid = c->mqid;
	i->mchan = c->mchan;
	i->ref = 1;
	l = &ihash(c->qid.path);
	i->hash = *l;
	*l = i;
found:
	unlock(&imagealloc);

	if(i->s == 0) {
		i->s = newseg(type, base, len);
		i->s->image = i;
	}
	else
		incref(i->s);

	return i;
}

void
imagereclaim(void)
{
	Page *p;

	if(!canqlock(&ireclaim))	/* Somebody is already cleaning the page cache */
		return;

	for(;;) {
		lock(&palloc);
		for(p = palloc.head; p; p = p->next)
			if(p->image)
			if(p->ref == 0)
			if(p->image != &swapimage)
				break;

		unlock(&palloc);
		if(p == 0)
			break;

		lockpage(p);
		if(p->ref == 0)
			uncachepage(p);
		unlockpage(p);
	}

	qunlock(&ireclaim);
}

void
putimage(Image *i)
{
	Image *f, **l;
	Chan *c;

	if(i == &swapimage)
		return;

	lock(i);
	if(--i->ref == 0) {
		l = &ihash(i->qid.path);
		i->qid = (Qid){~0, ~0};	
		unlock(i);
		c = i->c;
	
		lock(&imagealloc);
		for(f = *l; f; f = f->hash) {
			if(f == i) {
				*l = i->hash;
				break;
			}
			l = &f->hash;
		}

		i->next = imagealloc.free;
		imagealloc.free = i;
		unlock(&imagealloc);

		close(c);		/* Delay close because we could error */
		return;
	}
	unlock(i);
}

long
ibrk(ulong addr, int seg)
{
	Segment *s, *ns;
	ulong newtop, newsize;
	int i;

	s = u->p->seg[seg];
	if(s == 0)
		error(Ebadarg);

	if(addr == 0)
		return s->base;

	qlock(&s->lk);

	if(addr < s->base) {
		/* We may start with the bss overlapping the data */
		if(seg != BSEG || u->p->seg[DSEG] == 0 || addr < u->p->seg[DSEG]->base) {
			qunlock(&s->lk);
			error(Enovmem);
		}
		addr = s->base;
	}
		
	newtop = PGROUND(addr);
	newsize = (newtop-s->base)/BY2PG;
	if(newtop < s->top) {
		mfreeseg(s, newtop, (s->top-newtop)/BY2PG);
		qunlock(&s->lk);
		return 0;
	}

	if(newsize > (PTEMAPMEM*SEGMAPSIZE)/BY2PG) {
		qunlock(&s->lk);
		return -1;
	}

	for(i = 0; i < NSEG; i++) {
		ns = u->p->seg[i];
		if(ns == 0 || ns == s)
			continue;
		if(newtop >= ns->base)
		if(newtop < ns->top) {
			qunlock(&s->lk);
			pprint("segments overlap\n");
			error(Enovmem);
		}
	}

	s->top = newtop;
	s->size = newsize;

	qunlock(&s->lk);
	return 0;
}

void
mfreeseg(Segment *s, ulong start, int pages)
{
	int i, j;
	ulong soff;
	Page *pg;

	soff = start-s->base;
	j = (soff&(PTEMAPMEM-1))/BY2PG;

	for(i = soff/PTEMAPMEM; i < SEGMAPSIZE; i++) {
		if(pages <= 0) 
			goto done;
		if(s->map[i]) {
			while(j < PTEPERTAB) {
				if(pg = s->map[i]->pages[j]) {
					putpage(pg);
					s->map[i]->pages[j] = 0;	
				}
				if(--pages == 0)
					goto done;
				j++;
			}
		}
		else
			pages -= PTEPERTAB-j;
		j = 0;
	}
done:
	flushmmu();
}

ulong
segattach(Proc *p, ulong attr, char *name, ulong va, ulong len)
{
	Segment *s, *ns, *new;
	Physseg *ps;
	ulong newtop;
	int i, sno;

	USED(p);
	if(va&KZERO)					/* BUG: Only ok for now */
		error(Ebadarg);

	validaddr((ulong)name, 1, 0);
	vmemchr(name, 0, ~0);

	for(sno = 0; sno < NSEG; sno++)
		if(u->p->seg[sno] == 0)
		if(sno != ESEG)
			break;

	if(sno == NSEG)
		error(Enovmem);

	va = va&~(BY2PG-1);
	len = PGROUND(len);
	newtop = va+len;
	for(i = 0; i < NSEG; i++) {
		ns = u->p->seg[i];
		if(ns == 0)
			continue;	
		if((newtop > ns->base && newtop <= ns->top) ||
		   (va >= ns->base && va < ns->top))
			error(Enovmem);
	}

	for(ps = physseg; ps->name; ps++)
		if(strcmp(name, ps->name) == 0)
			goto found;

	error(Ebadarg);
found:
	if(len > ps->size)
		error(Enovmem);

	attr &= ~SG_TYPE;			/* Turn off what we are not allowed */
	attr |= ps->attr;			/* Copy in defaults */

	s = newseg(attr, va, len/BY2PG);
	s->pgalloc = ps->pgalloc;
	s->pgfree = ps->pgfree;
	u->p->seg[sno] = s;

	/* Need some code build mapped devices here */

	return 0;
}

long
syssegflush(ulong *arg)
{
	Segment *s;
	int i, j, pages;
	ulong soff;
	Page *pg;

	s = seg(u->p, arg[0], 1);
	if(s == 0)
		error(Ebadarg);

	s->flushme = 1;

	soff = arg[0]-s->base;
	j = (soff&(PTEMAPMEM-1))/BY2PG;
	pages = ((arg[0]+arg[1]+(BY2PG-1))&~(BY2PG-1))-(arg[0]&~(BY2PG-1));

	for(i = soff/PTEMAPMEM; i < SEGMAPSIZE; i++) {
		if(pages <= 0) 
			goto done;
		if(s->map[i]) {
			while(j < PTEPERTAB) {
				if(pg = s->map[i]->pages[j]) 
					memset(pg->cachectl, PG_TXTFLUSH, sizeof pg->cachectl);
				if(--pages == 0)
					goto done;
				j++;
			}
			j = 0;
		}
		else
			pages -= PTEMAPMEM/BY2PG;
	}
done:
	qunlock(&s->lk);
	flushmmu();
}

Page*
snewpage(ulong addr)
{
	return newpage(1, 0, addr);
}
