#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

/*
 * The hardware semaphores are strange.  64 per page, replicated 16 times
 * per page, 1024 pages of them.  Only the low bit is meaningful.
 * Reading an unset semaphore sets the semaphore and returns the old value.
 * Writing a semaphore sets the value, so writing 0 resets (clears) the semaphore.
 */

#define	SEMPERPG	64		/* hardware semaphores per page */
#define	NONSEMPERPG	(WD2PG-64)	/* words of non-semaphore per page */

struct
{
	Lock	lock;			/* lock to allocate */
	ulong	*nextsem;		/* next one to allocate */
	int	nsem;			/* at SEMPERPG, jump to next page */
}semalloc;

void
lockinit(void)
{
	semalloc.lock.sbsem = SBSEM;
	semalloc.nextsem = SBSEM+1;
	semalloc.nsem = 1;
	unlock(&semalloc.lock);
}

/*
 * If l->sbsem is zero, allocate a hardware semaphore first.
 * There is no way to free a semaphore.
 */
void
lock(Lock *l)
{
int addr;
	int i;
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(semalloc.nsem == SEMPERPG){
			semalloc.nsem = 0;
			semalloc.nextsem += NONSEMPERPG;
			if(semalloc.nextsem == SBSEMTOP)
				panic("sem");
		}
		l->sbsem = semalloc.nextsem;
		semalloc.nextsem++;
		semalloc.nsem++;
		unlock(&semalloc.lock);
		unlock(l);		/* put sem in known state */
		sbsem = l->sbsem;
	}
	/*
	 * Try the fast grab first
	 */
    	if((*sbsem&1) == 0){
		l->pc = ((ulong*)&addr)[-7];
		return;
	}
	for(i=0; i<10000000; i++)
    		if((*sbsem&1) == 0){
			l->pc = ((ulong*)&addr)[-7];
			return;
	}
	*sbsem = 0;
	print("lock loop %lux pc %lux held by pc %lux\n", l, ((ulong*)&addr)[-7], l->pc);
}

int
canlock(Lock *l)
{
	ulong *sbsem;

	sbsem = l->sbsem;
	if(sbsem == 0){
		lock(&semalloc.lock);
		if(semalloc.nsem == SEMPERPG){
			semalloc.nsem = 0;
			semalloc.nextsem += NONSEMPERPG;
			if(semalloc.nextsem == SBSEMTOP)
				panic("sem");
		}
		l->sbsem = semalloc.nextsem;
		semalloc.nextsem++;
		semalloc.nsem++;
		unlock(&semalloc.lock);
		unlock(l);		/* put sem in known state */
		sbsem = l->sbsem;
	}
	if(*sbsem & 1)
		return 0;
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	*l->sbsem = 0;
}

int
canqlock(QLock *q)
{
	return canlock(&q->use);
}

void
qlock(QLock *q)
{
	Proc *p;

	if(canlock(&q->use))
		return;
	lock(&q->queue);
	if(canlock(&q->use)){
		unlock(&q->queue);
		return;
	}
	p = q->tail;
	if(p == 0)
		q->head = u->p;
	else
		p->qnext = u->p;
	q->tail = u->p;
	u->p->qnext = 0;
	u->p->state = Queueing;
	unlock(&q->queue);
	sched();
}

void
qunlock(QLock *q)
{
	Proc *p;

	lock(&q->queue);
	if(q->head){
		p = q->head;
		q->head = p->qnext;
		if(q->head == 0)
			q->tail = 0;
		unlock(&q->queue);
		ready(p);
	}else{
		unlock(&q->use);
		unlock(&q->queue);
	}
}
