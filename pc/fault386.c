#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"

int faulting;

void
fault386(Ureg *ur)
{
	ulong addr;
	int read;
	int user;
	int n;
	static int times;

	addr = getcr2();
print("fault386 %lux ur %lux\n", addr, ur);
dumpregs(ur);
if(++times==3)
	panic("3rd time");
	if(faulting)
		panic("double fault\n");
	faulting = 1;
	read = !(ur->ecode & 2);
	user = (ur->ecode & 4);
	n = fault(addr, read);
print("fault returns %d\n", n);
	if(n < 0){
		if(user){
			pprint("user %s error addr=0x%lux\n", read? "read" : "write", addr);
			pprint("status=0x%lux pc=0x%lux sp=0x%lux\n", ur->flags, ur->pc, ur->usp);
			pexit("Suicide", 0);
		}
		u->p->state = MMUing;
		dumpregs(ur);
		panic("fault: 0x%lux", addr);
	}
	faulting = 0;
}

void
faultinit(void)
{
	setvec(Faultvec, fault386);
}
