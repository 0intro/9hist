#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include 	"arp.h"
#include 	"ipdat.h"

extern int tcpdbg;
#define DPRINT	if(tcpdbg) print

void
state_upcall(Ipconv *s, char oldstate, char newstate)
{
	Block *bp;
	int len;
	Tcpctl *tcb = &s->tcpctl;

	DPRINT("state_upcall: %s -> %s err %s\n", 
	      tcpstate[oldstate], tcpstate[newstate], s->err);

	if(oldstate == newstate)
		return;

	switch(newstate) {
	case Closed:
		s->psrc = 0;
		s->pdst = 0;
		s->dst = 0;
		/* NO break */
	case Close_wait:		/* Remote closes */
		if(s->err) {
			len = strlen(s->err);
			bp = allocb(len);
			strcpy((char *)bp->wptr, s->err);
			bp->wptr += len;
		}
		else
			bp = allocb(0);

		bp->flags |= S_DELIM;
		bp->type = M_HANGUP;
		qlock(s);
		if(waserror()) {
			qunlock(s);
			nexterror();
		}
		if(s->readq == 0){
			if(newstate == Close_wait)
				putb(&tcb->rcvq, bp);
			else
				freeb(bp);
		} else
			PUTNEXT(s->readq, bp);
		poperror();
		qunlock(s);
		break;
	}

	if(oldstate == Syn_sent)
		wakeup(&tcb->syner);
}

static int
notsyner(void *ic)
{
	return ((Tcpctl*)ic)->state != Syn_sent;
}

void
tcpstart(Ipconv *s, int mode, ushort window, char tos)
{
	Tcpctl *tcb = &s->tcpctl;

	if(tcb->state != Closed)
		return;

	init_tcpctl(s);

	tcb->window = tcb->rcv.wnd = window;
	tcb->tos = tos;

	switch(mode){
	case TCP_PASSIVE:
		tcb->flags |= CLONE;
		setstate(s, Listen);
		break;

	case TCP_ACTIVE:
		/* Send SYN, go into SYN_SENT state */
		tcb->flags |= ACTIVE;
		qlock(tcb);
		if(waserror()) {
			qunlock(tcb);
			nexterror();
		}
		send_syn(tcb);
		setstate(s, Syn_sent);
		tcp_output(s);
		poperror();
		qunlock(tcb);
		sleep(&tcb->syner, notsyner, tcb);
		if(tcb->state != Established && tcb->state != Syn_received)
			error(Etimedout);
		break;
	}
}
