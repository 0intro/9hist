#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
lockloop(Lock *l, ulong pc)
{
	print("lock loop key 0x%lux pc 0x%lux held by pc 0x%lux proc %d\n",
		l->key, pc, l->pc, l->pid);
	dumpaproc(up);
}

#define LOCKLOOP 100000000	/* to detect a lock loop */
#define SPINLOOP 10000000	/* to keep tas's off the bus */

void
lock(Lock *l)
{
	int i, pri, spins;
	ulong pc, pid;

	pc = getcallerpc(l);
	if(up){
		pid = up->pid;
		pri = up->priority;
	} else {
		pid = 0;
		pri = 0;
	}

	/* quick try, it might work */
	if(tas(&l->key) == 0){
		l->pc = pc;
		l->pid = pid;
		l->pri = pri;
		return;
	}

	spins = 0;
	for(;;){
		i = 0;
		while(l->key)
			if(i++ > SPINLOOP){
				/* look for lock loops */
				if(spins++ > LOCKLOOP/SPINLOOP){
					spins = 0;
					lockloop(l, pc);
				}

				/* possible priority inversion, try switching priority */
				if(up && up->state == Running)
				if(getstatus()&IE) {
print("priority inversion\n");
					up->lockpri = l->pri;
					sched();
				}
			}

		if(tas(&l->key) == 0){
			l->pc = pc;
			l->pid = pid;
			l->pri = pri;
			up->lockpri = 0;
			return;
		}
	}
}

void
ilock(Lock *l)
{
	ulong x;
	ulong pc, pid;

	pc = getcallerpc(l);
	pid = up ? up->pid : 0;

	x = splhi();
	if(tas(&l->key) == 0){
		l->sr = x;
		l->pc = pc;
		l->pid = pid;
		l->pri = 0;
		return;
	}

	for(;;){
		while(l->key)
			;
		if(tas(&l->key) == 0){
			l->sr = x;
			l->pc = pc;
			l->pid = pid;
			l->pri = 0;
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
	if(up){
		l->pid = up->pid;
		l->pri = up->priority;
	} else {
		l->pid = 0;
		l->pri = 0;
	}
	return 1;
}

void
unlock(Lock *l)
{
	l->key = 0;
	l->pc = 0;
	l->pri = 0;
}

void
iunlock(Lock *l)
{
	ulong sr;

	sr = l->sr;
	l->key = 0;
	l->pc = 0;
	l->pri = 0;
	splx(sr);
}
