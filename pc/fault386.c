#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"

void
fault386(Ureg *ur)
{
	ulong addr;
	int read;
	int user;

print("fault386\n");
dumpregs(ur);
for(;;);
	addr = getcr2();
	read = !(ur->ecode & 2);
	user = (ur->ecode & 4);
	if(fault(addr, read) < 0){
		if(user){
			pprint("user %s error addr=0x%lux\n", read? "read" : "write", addr);
			pprint("status=0x%lux pc=0x%lux sp=0x%lux\n", ur->flags, ur->pc, ur->usp);
			pexit("Suicide", 0);
		}
		u->p->state = MMUing;
		dumpregs(ur);
		panic("fault: 0x%lux", addr);
	}
}

void
faultinit(void)
{
	setvec(Faultvec, fault386);
}
