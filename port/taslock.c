#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
lock(Lock *l)
{
	int loop;

	if(tas(&l->key) == 0)
		return;

	loop = 50000000;
	for(;;){
		while(l->key) {
			if(loop-- <= 0) {
				dumpstack();
				panic("lock loop: lock %lux\n", l);
			}
		}
		if(tas(&l->key) == 0)
			return;
	}
}

void
ilock(Lock *l)
{
	ulong x;

	x = splhi();
	if(tas(&l->key) == 0){
		l->sr = x;
		return;
	}

	for(;;){
		while(l->key)
			;
		if(tas(&l->key) == 0){
			l->sr = x;
			return;
		}
	}
}

int
canlock(Lock *l)
{
	if(tas(&l->key))
		return 0;

	return 1;
}

void
unlock(Lock *l)
{
	l->key = 0;
}

void
iunlock(Lock *l)
{
	ulong sr;

	sr = l->sr;
	l->key = 0;
	splx(sr);
}
