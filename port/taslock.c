#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
lock(Lock *l)
{
	int i;
	ulong pc;

	pc = getcallerpc(l);

	if(tas(&l->key) == 0){
		l->pc = pc;
		return;
	}

	for(;;){
		i = 0;
		while(l->key)
			if(i++ > 100000000)
				panic("lock loop key 0x%lux pc 0x%lux held by pc 0x%lux pl 0x%lux\n",
					l->key, pc, l->pc, splhi());
		if(tas(&l->key) == 0){
			l->pc = pc;
			return;
		}
	}
}

void
ilock(Lock *l)
{
	ulong x;
	ulong pc;

	pc = getcallerpc(l);

	x = splhi();
	if(tas(&l->key) == 0){
		l->sr = x;
		l->pc = pc;
		return;
	}

	for(;;){
		while(l->key)
			;
		if(tas(&l->key) == 0){
			l->sr = x;
			l->pc = pc;
			return;
		}
	}
}

int
canlock(Lock *l)
{
	if(tas(&l->key))
		return 0;

	l->pc = getcallerpc(l);
	return 1;
}

void
unlock(Lock *l)
{
	l->key = 0;
	l->pc = 0;
}

void
iunlock(Lock *l)
{
	ulong sr;

	sr = l->sr;
	l->key = 0;
	l->pc = 0;
	splx(sr);
}
