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

int
canlock(Lock *l)
{
	int n;

	if(up) {
		n = up->inlock;
		up->inlock = n+2;
		if(tas(&l->key)) {
			up->inlock = n;
			return 0;
		}
		return 1;
	}

	if(tas(&l->key))
		return 0;
	return 1;
}

void
unlock(Lock *l)
{
	int n;

	if(up) {
		n = up->inlock-2;
		if(n < 0)
			n = 0;
		up->inlock = n;
	}
	l->key = 0;
}

void
ilock(Lock *l)
{
	ulong sr;

	sr = splhi();
	lock(l);
	l->sr = sr;
}

void
iunlock(Lock *l)
{
	ulong sr;

	sr = l->sr;
	unlock(l);
	splx(sr);
}
