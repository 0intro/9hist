#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"


static struct
{
	Lock;
	Timer	*ci;
}timers;

/*
 * called by clockintrsched()
 */
vlong
timernext(void)
{
	Timer *ci;
	vlong when;

	ilock(&timers);
	when = 0;
	ci = timers.ci;
	if(ci != nil)
		when = ci->when;
	iunlock(&timers);
	return when;
}

vlong
checktimer(Ureg *u, void*)
{
	Timer *ci;
	vlong when;

	ilock(&timers);
	while(ci = timers.ci){
		when = ci->when;
		if(when > fastticks(nil)){
			iunlock(&timers);
			return when;
		}
		timers.ci = ci->next;
		iunlock(&timers);
		(*ci->f)(u, ci);
		ilock(&timers);
	}
	iunlock(&timers);
	return 0;
}

void
timeradd(Timer *nci)
{
	Timer *ci, **last;

	ilock(&timers);
	last = &timers.ci;
	while(ci = *last){
		if(ci == nci){
			*last = ci->next;
			break;
		}
		last = &ci->next;
	}

	last = &timers.ci;
	while(ci = *last){
		if(ci->when > nci->when)
			break;
		last = &ci->next;
	}
	nci->next = *last;
	*last = nci;
	iunlock(&timers);
}

void
timerdel(Timer *dci)
{
	Timer *ci, **last;

	ilock(&timers);
	last = &timers.ci;
	while(ci = *last){
		if(ci == dci){
			*last = ci->next;
			break;
		}
		last = &ci->next;
	}
	iunlock(&timers);
}
