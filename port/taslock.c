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

void
lock(Lock *l)
{
	int pri, i;
	ulong pc;

	pc = getcallerpc(l);

	if(up == 0) {
		for(i=0; i<1000000; i++)
			if(tas(&l->key) == 0){
				l->pc = pc;
				return;
			}
		lockloop(l, pc);
		return;
	}

	pri = up->priority;
	up->priority = PriLock;

	for(i=0; i<1000; i++){
		if(tas(&l->key) == 0){
			l->pri = pri;
			l->pc = pc;
			return;
		}
		if(conf.nmach == 1 && up->state == Running && (getstatus()&IE))
			sched();
	}
	lockloop(l, pc);
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

	for(;;){
		while(l->key)
			;
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
	int pri;

	SET(pri);
	if(up) {
		pri = up->priority;
		up->priority = PriLock;
	}
	if(tas(&l->key)) {
		up->priority = pri;
		l->pc = getcallerpc(l);
		return 0;
	}
	l->pri = pri;
	return 1;
}

void
unlock(Lock *l)
{
	int p;

	p = l->pri;
	l->pc = 0;
	l->key = 0;
	if(up != 0)
		up->priority = p;
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
