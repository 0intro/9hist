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

	pc = getcallerpc(((uchar*)&l) - sizeof(l));

	for(i = 0; i < 20000000; i++){
    		if (tas(&l->key) == 0){
			l->pc = pc;
			return;
		}
	}
	panic("lock loop 0x%lux key 0x%lux pc 0x%lux held by pc 0x%lux\n",
			i, l->key, pc, l->pc);
}

int
canlock(Lock *l)
{
	if(tas(&l->key))
		return 0;
	l->pc = getcallerpc(((uchar*)&l) - sizeof(l));
	return 1;
}

void
unlock(Lock *l)
{
	l->pc = 0;
	l->key = 0;
}
