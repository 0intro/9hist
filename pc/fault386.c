#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"

static void
fault386(Ureg *ur, void *arg)
{
	ulong addr;
	int read;
	int user;
	int n;
	int insyscall;
	char buf[ERRLEN];

	USED(arg);

	if(up == 0){
		dumpregs(ur);
		for(;;);
	}

	insyscall = up->insyscall;
	up->insyscall = 1;
	addr = getcr2();
	read = !(ur->ecode & 2);
	user = (ur->cs&0xffff) == UESEL;
	spllo();
	n = fault(addr, read);
	if(n < 0){
		if(user){
			sprint(buf, "sys: trap: fault %s addr=0x%lux",
				read? "read" : "write", addr);
			postnote(up, 1, buf, NDebug);
			return;
		}
		dumpregs(ur);
		panic("fault: 0x%lux", addr);
	}
	up->insyscall = insyscall;
}

void
faultinit(void)
{
	setvec(Faultvec, fault386, 0);
}
