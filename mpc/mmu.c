#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


void
mmuinit(void)
{
	print("mmuinit\n");
	kernelmmu();
}

void
flushmmu(void)
{
//	print("flushmmu()\n");
	_flushmmu();
}

void
mmuswitch(Proc*)
{
	flushmmu();
}

void
mmurelease(Proc* proc)
{
	USED(proc);
}


void
putmmu(ulong va, ulong pa, Page *pg)
{
	int x, r;
	char *ctl;

//if((va&0x8000000) == 0)
//print("putmmu va=%ux pa=%ux\n", va, pa);
	x = splhi();
	r = _putmmu(va, pa);

	ctl = &pg->cachectl[m->machno];
	switch(*ctl) {
	default:
		panic("putmmu: %d\n", *ctl);
		break;
	case PG_NOFLUSH:
		break;
	case PG_TXTFLUSH:
		icflush((void*)pg->va, BY2PG);
		*ctl = PG_NOFLUSH;
		break;
	case PG_NEWCOL:
		dcflush((void*)pg->va, BY2PG);
		*ctl = PG_NOFLUSH;
		break;
	}

	splx(x);
}
