#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include 	"arp.h"
#include 	"ipdat.h"

/* Head of running timer chain */
Timer 	*timers;
QLock 	timerlock;
Rendez	Tcpack;
Rendez	tcpflowr;

void
tcpackproc(void *junk)
{
	Timer *t,*tp;
	Timer *expired;

	USED(junk);
	for(;;) {
		expired = 0;

		qlock(&timerlock);
		for(t = timers;t != 0; t = tp) {
			tp = t->next;
			if(tp == t)
				panic("Timer loop at %lux\n",(long)tp);
	
 			if(t->state == TIMER_RUN)
			if(--(t->count) == 0){

				/* Delete from active timer list */
				if(timers == t)
					timers = t->next;
				if(t->next != 0)
					t->next->prev = t->prev;
				if(t->prev != 0)
					t->prev->next = t->next;

				t->state = TIMER_EXPIRE;
				/* Put on head of expired timer list */
				t->next = expired;
				expired = t;
			}
		}
		qunlock(&timerlock);

		for(;;) {
			t = expired;
			if(t == 0)
				break;

			expired = t->next;
			if(t->state == TIMER_EXPIRE)
			if(t->func)
				(*t->func)(t->arg);
		}

		tsleep(&Tcpack, return0, 0, MSPTICK);
	}
}

void
start_timer(Timer *t)
{

	if(t == 0 || t->start == 0)
		return;

	qlock(&timerlock);

	t->count = t->start;
	if(t->state != TIMER_RUN){
		t->state = TIMER_RUN;
		/* Put on head of active timer list */
		t->prev = 0;
		t->next = timers;
		if(t->next != 0)
			t->next->prev = t;
		timers = t;
	}
	qunlock(&timerlock);
}

void
stop_timer(Timer *t)
{
	if(t == 0)
		return;

	qlock(&timerlock);

	if(t->state == TIMER_RUN){
		/* Delete from active timer list */
		if(timers == t)
			timers = t->next;
		if(t->next != 0)
			t->next->prev = t->prev;
		if(t->prev != 0)
			t->prev->next = t->next;
	}
	t->state = TIMER_STOP;

	qunlock(&timerlock);
}

void
tcpflow(void *conv)
{
	Ipconv *base, *ifc, *etab;

	base = (Ipconv*)conv;

	etab = &base[conf.ip];
	for(;;) {
		sleep(&tcpflowr, return0, 0);

		for(ifc = base; ifc < etab; ifc++) {
			if(ifc->readq)
			if(ifc->ref != 0)
			if(ifc->stproto == &tcpinfo)
			if(!QFULL(ifc->readq->next)) {
				tcprcvwin(ifc);
				tcp_acktimer(ifc);
			}
		}
	}
}

