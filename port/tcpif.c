#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include 	"arp.h"
#include 	"ipdat.h"

extern int tcpdbg;
#define DPRINT	if(tcpdbg) print

void
state_upcall(Ipconv *s, char oldstate, char newstate)
{
	Block *bp;
	int len;

	DPRINT("state_upcall: %s -> %s err %d\n", 
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
		if(s->readq == 0)
			break;

		if(s->err) {
			len = strlen(errstrtab[s->err]);
			bp = allocb(len);
			strcpy((char *)bp->wptr, errstrtab[s->err]);
			bp->wptr += len;
		}
		else
			bp = allocb(0);

		bp->flags |= S_DELIM;
		bp->type = M_HANGUP;
		PUTNEXT(s->readq, bp);
		break;
	}
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
		send_syn(tcb);
		setstate(s, Syn_sent);
		tcp_output(s);
		qunlock(tcb);
		break;
	}
}
