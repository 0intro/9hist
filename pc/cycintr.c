#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"


static struct
{
	Lock;
	vlong	when;			/* next scheduled interrupt time */
	Cycintr	*ci;
}cycintrs;

static void
cycsched(void)
{
}

void
checkcycintr(Ureg *u, void*)
{
	Cycintr *ci;

	ilock(&cycintrs);
	while(ci = cycintrs.ci){
		if(ci->when > fastticks(nil))
			break;
		cycintrs.ci = ci->next;
		iunlock(&cycintrs);
		(*ci->f)(u, ci);
		ilock(&cycintrs);
	}
	cycsched();
	iunlock(&cycintrs);
}

void
cycintradd(Cycintr *nci)
{
	Cycintr *ci, **last;

	ilock(&cycintrs);
	last = &cycintrs.ci;
	while(ci = *last){
		if(ci == nci){
			*last = ci->next;
			break;
		}
		last = &ci->next;
	}

	last = &cycintrs.ci;
	while(ci = *last){
		if(ci->when > nci->when)
			break;
		last = &ci->next;
	}
	nci->next = *last;
	*last = nci;
	if(nci->when < cycintrs.when)
		cycsched();
	iunlock(&cycintrs);
}

void
cycintrdel(Cycintr *dci)
{
	Cycintr *ci, **last;

	ilock(&cycintrs);
	last = &cycintrs.ci;
	while(ci = *last){
		if(ci == dci){
			*last = ci->next;
			break;
		}
		last = &ci->next;
	}
	iunlock(&cycintrs);
}
