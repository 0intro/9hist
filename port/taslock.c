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
				l->pri = 0;
				return;
			}
		lockloop(l, pc);
		return;
	}

	/* priority interacts with code in ready() in proc.c */
	pri = up->priority;

	for(i=0; i<1000000; i++){
		up->lockpri = l->pri;		/* assume priority of process holding lock */
		if(tas(&l->key) == 0){
			l->pri = pri;
			up->lockpri = 0;	/* back to normal priority */
			l->pc = pc;
			return;
		}
		if(conf.nmach == 1 && up->state == Running && (getstatus()&IE))
			sched();
	}
	lockloop(l, pc);
	up->lockpri = 0;	/* back to normal priority */
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

	if(up)
		pri = up->priority;
	else
		pri = 0;
	if(tas(&l->key)) {
		l->pc = getcallerpc(l);
		return 0;
	}
	l->pri = pri;
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key = 0;
	l->pri = 0;
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
