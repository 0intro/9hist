#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "errno.h"

#define PCOFF -1

/*
 *  N.B.  Ken's compiler generates a TAS instruction for the sequence:
 *
 *  	if(l->key >= 0){
 *		l->key |= 0x80;
 *		...
 *
 *	DO NOT TAKE THE ADDRESS OF l->key or the TAS will disappear.
 */
void
lock(Lock *l)
{
	Lock *ll = l;	/* do NOT take the address of ll */
	int i;

	/*
	 * Try the fast grab first
	 */
    	if(ll->key >= 0){
		ll->key |= 0x80;
		ll->pc = ((ulong*)&l)[PCOFF];
		if(u && u->p)
			u->p->hasspin = 1;
		return;
	}
	for(i=0; i<10000000; i++)
    		if(ll->key >= 0){
			ll->key |= 0x80;
			ll->pc = ((ulong*)&l)[PCOFF];
			if(u && u->p)
				u->p->hasspin = 1;
			return;
		}
	ll->key = 0;
	print("lock loop %lux pc %lux held by pc %lux\n", l, ((ulong*)&l)[PCOFF], l->pc);
}

int
canlock(Lock *l)
{
	Lock *ll = l;	/* do NOT take the address of ll */
	if(ll->key >= 0){
		ll->key |= 0x80;
		ll->pc = ((ulong*)&l)[PCOFF];
		if(u && u->p)
			u->p->hasspin = 1;
		return 1;
	}
	return 0;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key = 0;
	if(u && u->p)
		u->p->hasspin = 0;
}
