#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include 	"arp.h"
#include 	"ipdat.h"

extern int tcpdbg;
extern ushort tcp_mss;
int tcptimertype = 0;


#define DPRINT if(tcpdbg) print

void
tcp_output(Ipconv *s)
{
	Block *hbp,*dbp, *sndq;
	ushort ssize, dsize, usable, sent, oobsent;
	int qlen;
	char doing_oob;	
	Tcphdr ph;
	Tcp seg;
	Tcpctl *tcb;

	tcb = &s->tcpctl;

	switch(tcb->state) {
	case LISTEN:
	case CLOSED:
		return;
	}
	for(;;){
		if (tcb->sndoobq) {
			/* We have pending out-of-band data - use it */
			qlen = tcb->sndoobcnt;
			oobsent = tcb->snd.ptr - tcb->snd.up;
			if (oobsent >= qlen) {
				oobsent = qlen;
				goto normal;
			}
			sndq = tcb->sndoobq;
			sent = oobsent;
			doing_oob = 1;
			DPRINT("tcp_out: oob: qlen = %lux sent = %lux\n",
						qlen, sent);
		} else {
			oobsent = 0;
			normal:
			qlen = tcb->sndcnt;
			sent = tcb->snd.ptr - tcb->snd.una - oobsent;
			sndq = tcb->sndq;
			doing_oob = 0;
			DPRINT("tcp_out: norm: qlen = %lux sent = %lux\n", qlen, sent);
		}

		/* Don't send anything else until our SYN has been acked */
		if(sent != 0 && !(tcb->flags & SYNACK))
			break;

		if(tcb->snd.wnd == 0){
			/* Allow only one closed-window probe at a time */
			if(sent != 0)
				break;
			/* Force a closed-window probe */
			usable = 1;
		} else {
			/* usable window = offered window - unacked bytes in transit
			 * limited by the congestion window
			 */
			usable = MIN(tcb->snd.wnd,tcb->cwind) - sent;
			if(sent != 0 && qlen - sent < tcb->mss) 
				usable = 0;
		}

		ssize = MIN(qlen - sent, usable);
		ssize = MIN(ssize, tcb->mss);
		dsize = ssize;

		if (!doing_oob)
			seg.up = 0;
		else {
			seg.up = ssize;
			DPRINT("tcp_out: oob seg.up = %d\n", seg.up);
		}

		DPRINT("tcp_out: ssize = %lux\n", ssize);
		if(ssize == 0 && !(tcb->flags & FORCE))
			break;

		/* Stop ack timer if one will be piggy backed on data */
		stop_timer(&tcb->acktimer);

		tcb->flags &= ~FORCE;
		tcprcvwin(s);

		seg.source = s->psrc;
		seg.dest = s->pdst;
		/* Every state except SYN_SENT */
		seg.flags = ACK; 	
		seg.mss = 0;

		switch(tcb->state){
		case SYN_SENT:
			seg.flags = 0;
			/* No break */
		case SYN_RECEIVED:
			if(tcb->snd.ptr == tcb->iss){
				seg.flags |= SYN;
				dsize--;
				/* Also send MSS */
				seg.mss = tcp_mss;
			}
			break;
		}
		seg.seq = tcb->snd.ptr;
		seg.ack = tcb->last_ack = tcb->rcv.nxt;
		seg.wnd = tcb->rcv.wnd;

		if (doing_oob) {
			DPRINT("tcp_out: Setting URG (up = %u)\n", seg.up);
			seg.flags |= URG;
		}

		/* Now try to extract some data from the send queue.
		 * Since SYN and FIN occupy sequence space and are reflected
		 * in sndcnt but don't actually sit in the send queue,
		 * dupb will return one less than dsize if a FIN needs to be sent.
		 */
		if(dsize != 0){
			if(dupb(&dbp, sndq, sent, dsize) != dsize) {
				seg.flags |= FIN;
				dsize--;
			}
			DPRINT("dupb: 1st char = %c\n", dbp->rptr[0]);
		} else
			dbp = 0;

		/* If the entire send queue will now be in the pipe, set the
		 * push flag
		 */
		if((dsize != 0) && 
		   ((sent + ssize) == (tcb->rcvcnt + tcb->rcvoobcnt)))
			seg.flags |= PSH;

		/* If this transmission includes previously transmitted data,
		 * snd.nxt will already be past snd.ptr. In this case,
		 * compute the amount of retransmitted data and keep score
		 */
		if(tcb->snd.ptr < tcb->snd.nxt)
			tcb->resent += MIN((int)tcb->snd.nxt - (int)tcb->snd.ptr,(int)ssize);

		tcb->snd.ptr += ssize;

		/* If this is the first transmission of a range of sequence
		 * numbers, record it so we'll accept acknowledgments
		 * for it later
		 */
		if(seq_gt(tcb->snd.ptr,tcb->snd.nxt))
			tcb->snd.nxt = tcb->snd.ptr;

		/* Fill in fields of pseudo IP header */
		hnputl(ph.tcpdst, s->dst);
		hnputl(ph.tcpsrc, Myip);
		hnputs(ph.tcpsport, s->psrc);
		hnputs(ph.tcpdport, s->pdst);

		/* Build header, link data and compute cksum */
		if((hbp = htontcp(&seg, dbp, &ph)) == 0) {
			freeb(dbp);
			return;
		}

		/* If we're sending some data or flags, start retransmission
		 * and round trip timers if they aren't already running.
		 */
		if(ssize != 0){
			tcb->timer.start = backoff(tcb->backoff) *
			 (2 * tcb->mdev + tcb->srtt + MSPTICK) / MSPTICK;
			if(!run_timer(&tcb->timer))
				start_timer(&tcb->timer);

			/* If round trip timer isn't running, start it */
			if(!run_timer(&tcb->rtt_timer)){
				start_timer(&tcb->rtt_timer);
				tcb->rttseq = tcb->snd.ptr;
			}
		}
		DPRINT("tcp_output: ip_send s%lux a%lux w%lux u%lux\n",
			seg.seq, seg.ack, seg.wnd, seg.up);

		PUTNEXT(Ipoutput, hbp);
	}
}

void
tcp_timeout(void *arg)
{
	Tcpctl *tcb;
	Ipconv *s;

	s = (Ipconv *)arg;
	tcb = &s->tcpctl;

	DPRINT("Timer %lux state = %d\n", s, tcb->state);

	switch(tcb->state){
	case CLOSED:
		panic("tcptimeout");
	case TIME_WAIT:
		close_self(s, 0);
		break;
	case ESTABLISHED:
		if(tcb->backoff < MAXBACKOFF)
			tcb->backoff++;
		goto retran;
	default:
		tcb->backoff++;
		DPRINT("tcp_timeout: retransmit %d %x\n", tcb->backoff, s);

		if (tcb->backoff >= MAXBACKOFF) {
			DPRINT("tcp_timeout: timeout\n");
			close_self(s, Etimedout);
		}
		else {
	retran:
			qlock(tcb);
			tcb->flags |= RETRAN|FORCE;
			tcb->snd.ptr = tcb->snd.una;

			/* Reduce slowstart threshold to half current window */
			tcb->ssthresh = tcb->cwind / 2;
			tcb->ssthresh = MAX(tcb->ssthresh,tcb->mss);

			/* Shrink congestion window to 1 packet */
			tcb->cwind = tcb->mss;
			tcp_output(s);
			qunlock(tcb);
		}
	}
}

int
backoff(int n)
{
	if(tcptimertype == 1) 
		return n+1;
	else {
		if(n <= 4)
			return 1 << n;
		else
			return n * n;
	}
}

void
tcp_acktimer(Ipconv *s)
{
	Tcpctl *tcb = &s->tcpctl;

	qlock(tcb);
	tcb->flags |= FORCE;
	tcprcvwin(s);
	tcp_output(s);
	qunlock(tcb);
}

void
tcprcvwin(Ipconv *s)
{
	Tcpctl *tcb = &s->tcpctl;

	/* Calculate new window */
	if(s->readq) {
		tcb->rcv.wnd = Streamhi - s->readq->next->len;
		if(tcb->rcv.wnd < 0)
			tcb->rcv.wnd = 0;
	}
	else
		tcb->rcv.wnd = Streamhi;
}

/*
 * Network byte order functions
 */

void
hnputs(uchar *ptr, ushort val)
{
	ptr[0] = val>>8;
	ptr[1] = val;
}

void
hnputl(uchar *ptr, ulong val)
{
	ptr[0] = val>>24;
	ptr[1] = val>>16;
	ptr[2] = val>>8;
	ptr[3] = val;
}

ulong
nhgetl(uchar *ptr)
{
	return ((ptr[0]<<24) | (ptr[1]<<16) | (ptr[2]<<8) | ptr[3]);
}

ushort
nhgets(uchar *ptr)
{
	return ((ptr[0]<<8) | ptr[1]);
}
