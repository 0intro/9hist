#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void
lock(Lock *l)
{
	int i;

	/*
	 * Try the fast grab first
	 */
    	if(tas(l->key) == 0){
		l->pc = ((ulong*)&l)[-1];
		return;
	}
	for(i=0; i<10000000; i++)
    		if(tas(l->key) == 0){
			l->pc = ((ulong*)&l)[-1];
			return;
	}
	l->key[0] = 0;
	panic("lock loop %lux pc %lux held by pc %lux\n", l, ((ulong*)&l)[-1], l->pc);
}

int
canlock(Lock *l)
{
	if(tas(l->key) == 0){
		l->pc = ((ulong*)&l)[-1];
		return 1;
	}
	return 0;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key[0] = 0;
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
