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
	char buf[ERRLEN];
	int user, read;
	ulong tbr;

	tbr = (ur->tbr&0xFFF)>>4;
	addr = ur->pc;			/* assume instr. exception */
	read = 1;
	if(tbr == 9){			/* data access exception */
		/*
		 * According to the book, this isn't good enough.  We'll see.
		 */
		addr = getw2(SEVAR);
		if(getw2(SER) & 0x8000)
			read = 0;
	}
	spllo();
	if(u == 0){
		dumpregs(ur);
		panic("fault u==0 pc=%lux addr=%lux", ur->pc, addr);
	}
/*	addr &= VAMASK; /**/
	badvaddr = addr;
	addr &= ~(BY2PG-1);
	user = !(ur->psr&PSRPSUPER);

	if(fault(addr, read) < 0){
		if(user){
			sprint(buf, "sys: fault %s pc=0x%lux addr=0x%lux",
				read? "read" : "write", ur->pc, badvaddr);
			postnote(u->p, 1, buf, NDebug);
			notify(ur);
			return;
		}
		dumpregs(ur);
		panic("fault: 0x%lux", badvaddr);
		exit();
	}
	splhi();
}

void
faultasync(Ureg *ur)
{
	int user;

	print("interrupt 15 ASER %lux ASEVAR %lux SER %lux\n", getw2(ASER), getw2(ASEVAR), getw2(SER));
	dumpregs(ur);
	/*
	 * Clear interrupt by toggling low bit of interrupt register
	 */
	*intrreg &= ~1;
	*intrreg |= 1;
	user = !(ur->psr&PSRPSUPER);
	if(user)
		pexit("Suicide", 0);
	panic("interrupt 15");
}

/*
 * called in sysfile.c
 */
void
evenaddr(ulong addr)
{
	if(addr & 3){
		postnote(u->p, 1, "sys: odd address", NDebug);
		error(Ebadarg);
	}
}
