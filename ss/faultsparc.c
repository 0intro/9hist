#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"errno.h"


void
faultsparc(Ureg *ur)
{
	ulong addr, badvaddr;
	int user, read, insyscall;
	ulong tbr;

	tbr = (ur->tbr&0xFFF)>>4;
	addr = ur->pc;			/* assume instr. exception */
	if(tbr == 9)			/* data access exception */
		addr = getw2(SEVAR);
	else if(tbr != 1){		/* should be instruction access exception */
		trap(ur);
		return;
	}
	spllo();
print("fault: %s pc=0x%lux addr %lux\n", excname(tbr), ur->pc, addr);
	if(u == 0){
		dumpregs(ur);
		panic("fault u==0 pc=%lux", ur->pc);
	}
	if(getw2(SER) & 0x8000)
		read = 0;
	else
		read = 1;
	insyscall = u->p->insyscall;
	u->p->insyscall = 1;
/*	addr &= VAMASK; /**/
	badvaddr = addr;
	addr &= ~(BY2PG-1);
	user = !(ur->psr&PSRSUPER);

	if(fault(addr, read) < 0){
		if(user){
			pprint("user %s error addr=0x%lux\n", read? "read" : "write", badvaddr);
			pprint("psr=0x%lux pc=0x%lux sp=0x%lux\n", ur->psr, ur->pc, ur->sp);
			pexit("Suicide", 0);
		}
		u->p->state = MMUing;
		dumpregs(ur);
		panic("fault: 0x%lux", badvaddr);
		exit();
	}
	u->p->insyscall = insyscall;
}

/*
 * called in sysfile.c
 */
void
evenaddr(ulong addr)
{
	if(addr & 3){
panic("evenaddr");
		postnote(u->p, 1, "sys: odd address", NDebug);
		error(Ebadarg);
	}
}
