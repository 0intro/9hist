#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"

void
faultinit(void)
{
	setvec(Faultvec, fault386, SEGTG);
}

void
fault386(Ureg *ur)
{
	panic("fault");
}
