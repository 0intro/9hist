#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#define PCOFF -1

void
lock(Lock *l)
{
	int i;

	for(i = 0; i < 1000000; i++){
    		if (tas(&l->key) == 0){
			if(u)
				u->p->hasspin = 1;
			l->pc = ((ulong*)&l)[PCOFF];
			return;
		}
		if(u && u->p->state == Running)
			sched();
	}
	i = l->key;
	l->key = 0;

	panic("lock loop 0x%lux key 0x%lux pc 0x%lux held by pc 0x%lux\n", l, i,
		((ulong*)&l)[PCOFF], l->pc);
}

int
canlock(Lock *l)
{
	if(tas(&l->key))
		return 0;
	l->pc = ((ulong*)&l)[PCOFF];
	if(u && u->p)
		u->p->hasspin = 1;
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key = 0;
	if(u && u->p)
		u->p->hasspin = 0;
}
