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

	/* lower priority till we get the lock */
	if(up && up->state == Running && islo()){
		up->lockpri = 1;
		sched();
	}
}

void
lock(Lock *l)
{
	int i;
	ulong pc, pid;

	pc = getcallerpc(l);
	pid = up ? up->pid : 0;

	if(tas(&l->key) == 0){
		l->pc = pc;
		l->pid = pid;
		return;
	}

	for(;;){
		i = 0;
		while(l->key)
			if(i++ > 100000000){
				i = 0;
				lockloop(l, pc);
			}
		if(tas(&l->key) == 0){
			l->pc = pc;
			l->pid = pid;
			if(up)
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
		return;
	}

	if(conf.nmach < 2)
		panic("ilock: no way out: pc %uX\n", pc);

	for(;;){
		splx(x);
		while(l->key)
			;
		x = splhi();
		if(tas(&l->key) == 0){
			l->sr = x;
			l->pc = pc;
			l->pid = pid;
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
	l->pid = up ? up->pid : 0;
	return 1;
}

void
unlock(Lock *l)
{
	if(l->key == 0)
		print("unlock: not locked: pc %uX\n", getcallerpc(l));
	l->pc = 0;
	l->key = 0;
	coherence();
}

void
iunlock(Lock *l)
{
	ulong sr;

	if(l->key == 0)
		print("iunlock: not locked: pc %uX\n", getcallerpc(l));

	sr = l->sr;
	l->pc = 0;
	l->key = 0;
	splx(sr);
	coherence();
}
