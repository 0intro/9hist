#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
lock(Lock *l)
{
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
