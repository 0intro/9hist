#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#define PCOFF -1

/*
 *  N.B.  Ken's compiler generates a TAS instruction for the sequence:
 *
 *  	if(l->key >= 0){
 *		l->key |= 0x80;
 *		...
 *
 *	DO NOT TAKE THE ADDRESS OF l->key or the TAS will disappear.
 */
void
lock(Lock *l)
{
	Lock *ll = l;	/* do NOT take the address of ll */
	int i;

	/*
	 * Try the fast grab first
	 */
    	if(ll->key >= 0){
		ll->key |= 0x80;
		ll->pc = ((ulong*)&l)[PCOFF];
		return;
	}
	for(i=0; i<10000000; i++)
    		if(ll->key >= 0){
			ll->key |= 0x80;
			ll->pc = ((ulong*)&l)[PCOFF];
			return;
		}
	ll->key = 0;
dumpstack();
	panic("lock loop %lux pc %lux held by pc %lux\n", l, ((ulong*)&l)[PCOFF], l->pc);
}

int
canlock(Lock *l)
{
	Lock *ll = l;	/* do NOT take the address of ll */
	if(ll->key >= 0){
		ll->key |= 0x80;
		ll->pc = ((ulong*)&l)[PCOFF];
		return 1;
	}
	return 0;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key = 0;
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
	u->p->qlock = q;	/* DEBUG */
	unlock(&q->queue);
	sched();
}

int
canqlock(QLock *q)
{
	return canlock(&q->use);
}

void
qunlock(QLock *q)
{
	Proc *p;

	lock(&q->queue);
	u->p->qlock = 0;
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
