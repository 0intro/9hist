#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	"ureg.h"

void
delay(int ms)
{
	ulong t, *p;
	int i;

	ms *= 1000;	/* experimentally determined */
	for(i=0; i<ms; i++)
		;
}

void
clock(Ureg *ur)
{
	Proc *p;

	SYNCREG[1] = 0x5F;	/* clear interrupt */
	m->ticks++;
	p = m->proc;
	if(p){
		p->pc = ur->pc;
		if (p->state==Running)
			p->time[p->insyscall]++;
	}
	checkalarms();
	kbdclock();
	mouseclock();
	duartclock();
	if((ur->sr&SPL(7)) == 0 && p && p->state==Running){
		if(anyready()){
			if(p->hasspin)
				p->hasspin = 0;
			else
				sched();
		}
		if((ur->sr&SUPER) == 0){
			(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);	/* profiling clock */
			if(u->nnote)
				notify(ur);
		}
	}
}
