#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
lock(Lock *l)
{
	int n;

	if(up) {
		n = up->inlock+2;
		up->inlock = n;
		if(tas(&l->key) == 0)
			return;

		for(;;){
			while(l->key)
				if(conf.nproc == 1) {
					up->yield = 1;
					sched();
				}
			up->inlock = n;
			if(tas(&l->key) == 0)
				return;
		}
	}

	if(tas(&l->key) == 0)
		return;
	for(;;){
		while(l->key)
			;
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
	int n;

	l->key = 0;
	if(up) {
		n = up->inlock-2;
		if(n < 0)
			n = 0;
		up->inlock = n;
	}
}

void
iunlock(Lock *l)
{
	ulong sr;

	sr = l->sr;
	l->key = 0;
	splx(sr);
}
