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
		l->key, pc, l->pc, l->p ? l->p->pid : 0);
	dumpaproc(up);

	if(up && up->state == Running && islo())
		sched();
}

void
lock(Lock *l)
{
	int i;
	ulong pc;

	pc = getcallerpc(l);

	if(tas(&l->key) == 0){
		l->pc = pc;
		l->p = up;
		l->isilock = 0;
		if(up){
			l->pri = up->priority;
			up->priority = PriLock;
		}
		return;
	}

	for(;;){
		i = 0;
		while(l->key){
			if(conf.nmach < 2){
				if(i++ > 1000){
					i = 0;
					lockloop(l, pc);
				}
				sched();
			} else {
				if(i++ > 100000000){
					i = 0;
					lockloop(l, pc);
				}
			}
		}
		if(tas(&l->key) == 0){
			l->pc = pc;
			l->p = up;
			l->isilock = 0;
			if(up){
				l->pri = up->priority;
				up->priority = PriLock;
			}
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
		l->p = up;
		l->isilock = 1;
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
			l->p = up;
			l->isilock = 1;
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
	l->p = up;
	l->isilock = 0;
	if(up){
		l->pri = up->priority;
		up->priority = PriLock;
	}
	return 1;
}

void
unlock(Lock *l)
{
	int p;

	p = l->pri;
	if(l->key == 0)
		print("unlock: not locked: pc %uX\n", getcallerpc(l));
	if(l->isilock)
		print("iunlock of lock: pc %lux, held by %lux\n", getcallerpc(l), l->pc);
	l->pc = 0;
	l->key = 0;
	if(up && p < up->priority)
		up->priority = p;
	coherence();
}

void
iunlock(Lock *l)
{
	ulong sr;

	if(l->key == 0)
		print("iunlock: not locked: pc %uX\n", getcallerpc(l));
	if(!l->isilock)
		print("unlock of ilock: pc %lux, held by %lux\n", getcallerpc(l), l->pc);

	sr = l->sr;
	l->pc = 0;
	l->key = 0;
	splx(sr);
	coherence();
}
