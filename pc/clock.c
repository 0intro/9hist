#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

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
clock(void *arg)
{
	m->ticks++;
	if((m->ticks%185)==0)
		print("%d secs\n", TK2SEC(m->ticks));
	INT0ENABLE;
}
