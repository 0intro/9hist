#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

/*
 *  8253 timer
 */
enum
{
	Timerctl=	0x43,		/* control port */
	Timercnt=	0x40,		/* timer count port (outb count-1) */
	Timericnt=	0x41,		/* timer count input port */

	Timerlatch=	0x40,		/* latch count into Timericnt */
};

void
clockinit(void)
{
	setvec(Clockvec, clock, SEGIG);
}

void
clock(Ureg *ur)
{
	Proc *p;

	m->ticks++;
	p = m->proc;
	if(p){
		p->pc = ur->eip;
		if (p->state==Running)
			p->time[p->insyscall]++;
	}

	if((m->ticks%185) == 92)
		floppystart();
	else if((m->ticks%185) == 0)
		floppystop();
}
