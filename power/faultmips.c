#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"../port/error.h"
#include	"io.h"

/*
 *  find out fault address and type of access.
 *  Call common fault handler.
 */
void
faultmips(Ureg *ur, int user, int code)
{
	int read;
	ulong addr;
	extern char *excname[];
	char *p, buf[ERRLEN];

	addr = ur->badvaddr;
	addr &= ~(BY2PG-1);
	read = !(code==CTLBM || code==CTLBS);

	if(fault(addr, read) == 0)
		return;

	if(user) {
		p = "store";
		if(read)
			p = "load";
		sprint(buf, "sys: trap: fault %s addr=0x%lux", p, ur->badvaddr);
		postnote(up, 1, buf, NDebug);
		return;
	}
	print("kernel %s vaddr=0x%lux\n", excname[code], ur->badvaddr);
	print("status=0x%lux pc=0x%lux sp=0x%lux\n", ur->status, ur->pc, ur->sp);
	dumpregs(ur);
	panic("fault");
}

/*
 * called in sysfile.c
 */
void
evenaddr(ulong addr)
{
	if(addr & 3){
		postnote(up, 1, "sys: odd address", NDebug);
		error(Ebadarg);
	}
}
