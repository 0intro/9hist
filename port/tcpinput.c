#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include 	"arp.h"
#include 	"ipdat.h"

int tcpdbg = 0;
#define DPRINT	if(tcpdbg) print
#define LPRINT  if(tcpdbg) print

QLock	reseqlock;
Reseq	*reseqfree;

char *tcpstate[] = {
	"Closed", 	"Listen", 	"Syn_sent", "Syn_received",
	"Established", 	"Finwait1",	"Finwait2", "Close_wait",
	"Closing", 	"Last_ack", 	"Time_wait" };

void
tcpinit(void)
{
	Reseq *r;

	reseqfree = ialloc(sizeof(Reseq)*Nreseq, 0);
	for(r = reseqfree; r < &reseqfree[Nreseq-1]; r++)
		r->next = r+1;

	r->next = 0;
}

void
reset(Ipaddr source, Ipaddr dest, char tos, ushort length, Tcp *seg)
{
	Block *hbp;
	Port tmp;
	char rflags;
	Tcphdr ph;

	if(seg->flags & RST)
		return;

	hnputl(ph.tcpsrc, dest);
	hnputl(ph.tcpdst, source);
	ph.proto = IP_TCPPROTO;
	hnputs(ph.tcplen, TCP_HDRSIZE);

	/* Swap port numbers */
	tmp = seg->dest;
	seg->dest = seg->source;
	seg->source = tmp;

	rflags = RST;

	/* convince the other end that this reset is in band */
	if(seg->flags & ACK) {
		seg->seq = seg->ack;
		seg->ack = 0;
	}
	else {
		rflags |= ACK;
		seg->ack = seg->seq;
		seg->seq = 0;
		if(seg->flags & SYN)
			seg->ack++;
		seg->ack += length;
		if(seg->flags & FIN)
			seg->ack++;
	}
	seg->flags = rflags;
	seg->wnd = 0;
	seg->up = 0;
	seg->mss = 0;
	if((hbp = htontcp(seg, 0, &ph)) == 0)
		return;

	PUTNEXT(Ipoutput, hbp);
}

Ipconv*
tcpincoming(Ipconv *ipc, Ipconv *s, Tcp *segp, Ipaddr source)
{
	Ipconv *new;

	if(s->curlog >= s->backlog)
		return 0;

	new = ipincoming(ipc, s);
	if(new == 0)
		return 0;

	qlock(s);
	s->curlog++;
	qunlock(s);
	new->psrc = segp->dest;
	new->pdst = segp->source;
	new->dst = source;
	memmove(&new->tcpctl, &s->tcpctl, sizeof(Tcpctl));
	new->tcpctl.flags &= ~CLONE;
	new->tcpctl.timer.arg = new;
	new->tcpctl.timer.state = TIMER_STOP;
	new->tcpctl.acktimer.arg = new;
	new->tcpctl.acktimer.state = TIMER_STOP;
	new->newcon = 1;
	new->ipinterface = s->ipinterface;

	s->ipinterface->ref++;
	wakeup(&s->listenr);
	return new;
}

void
tcp_input(Ipconv *ipc, Block *bp)
{
	Ipconv *s, *e;
	Ipconv *spec, *gen;
	Tcpctl *tcb;		
	Tcphdr *h;
	Tcp seg;
	int hdrlen;	
	Ipaddr source, dest;
	char tos;
	ushort length;

	h = (Tcphdr *)(bp->rptr);
	dest = nhgetl(h->tcpdst);
	source = nhgetl(h->tcpsrc);

	tos = h->tos;
	length = nhgets(h->length);

	h->Unused = 0;
	hnputs(h->tcplen, length - (TCP_IPLEN+TCP_PHDRSIZE));
	if(ptcl_csum(bp, TCP_EHSIZE+TCP_IPLEN, length - TCP_IPLEN)) {
		freeb(bp);
		return;
	}

	if((hdrlen = ntohtcp(&seg, &bp)) < 0)
		return;

	/* Adjust the data length */
	length -= (hdrlen+TCP_IPLEN+TCP_PHDRSIZE);
	bp = btrim(bp, hdrlen+TCP_PKT, length);
	if(bp == 0)
		return;
	
	/* Look for a connection, failing that attempt to establish a listen */
	s = ip_conn(ipc, seg.dest, seg.source, source, IP_TCPPROTO);
	if (s == 0) {
		if(seg.flags & SYN){
			/*
			 *  find a listener specific to this port (spec) or,
			 *  failing that, a general one (gen)
			 */
			spec = 0;
			gen = 0;
			e = &ipc[conf.ip];
			for(s = ipc; s < e; s++) {
				if(s->tcpctl.state == Listen)
				if(s->pdst == 0)
				if(s->dst == 0) {
					if(s->psrc == seg.dest){
						spec = s;
						break;
					}
					if(s->psrc == 0)
						gen = s;
				}
			}
			if(spec)
				s = tcpincoming(ipc, spec, &seg, source);
			else if(gen)
				s = tcpincoming(ipc, gen, &seg, source);
			else
				s = 0;
		}
		if(s == 0){
			freeb(bp);   
			reset(source, dest, tos, length, &seg);
			return;
		}
	}

	/* The rest of the input state machine is run with the control block
	 * locked
	 */
	tcb = &s->tcpctl;
	qlock(tcb);

	switch(tcb->state) {
	case Closed:
		freeb(bp);
		reset(source, dest, tos, length, &seg);
		goto done;
	case Listen:
		if(seg.flags & RST) {
			freeb(bp);
			goto done;
		} 
		if(seg.flags & ACK) {
			freeb(bp);
			reset(source, dest, tos, length, &seg);
			goto done;
		}
		if(seg.flags & SYN) {
			proc_syn(s, tos, &seg);
			send_syn(tcb);
			setstate(s, Syn_received);		
			if(length != 0 || (seg.flags & FIN)) 
				break;
			freeb(bp);
			goto output;
		}
		freeb(bp);
		goto done;
	case Syn_sent:
		if(seg.flags & ACK) {
			if(!seq_within(seg.ack, tcb->iss+1, tcb->snd.nxt)) {
				freeb(bp);
				reset(source, dest, tos, length, &seg);
				goto done;
			}
		}
		if(seg.flags & RST) {
			if(seg.flags & ACK)
				close_self(s, Econrefused);
			freeb(bp);
			goto done;
		}

		if(seg.flags & ACK)
		if(PREC(tos) != PREC(tcb->tos)){
			freeb(bp);
			reset(source, dest, tos, length, &seg);
			goto done;
		}

		if(seg.flags & SYN) {
			proc_syn(s, tos, &seg);
			if(seg.flags & ACK){
				update(s, &seg);
				setstate(s, Established);
			}
			else 
				setstate(s, Syn_received);

			if(length != 0 || (seg.flags & FIN))
				break;

			freeb(bp);
			goto output;
		}
		else 
			freeb(bp);
		goto done;
	}

	/* Trim segment to fit receive window. */
	if(trim(tcb, &seg, &bp, &length) == -1) {
		if(!(seg.flags & RST)) {
			tcb->flags |= FORCE;
			goto output;
		}
		goto done;
	}

	/* If we dont understand answer with a rst */
	if(length)
	if(s->readq == 0) {
		freeb(bp);
		reset(source, dest, tos, length, &seg);
		goto done;
	}

	if(seg.seq != tcb->rcv.nxt)
	if(length != 0 || (seg.flags & (SYN|FIN))) {
		add_reseq(tcb, tos, &seg, bp, length);
		tcb->flags |= FORCE;
		goto output;
	}

	for(;;) {
		if(seg.flags & RST) {
			if(tcb->state == Syn_received
			   && !(tcb->flags & (CLONE|ACTIVE))) 
				setstate(s, Listen);
			else
				close_self(s, Econrefused);

			freeb(bp);
			goto done;
		}

		if(PREC(tos) != PREC(tcb->tos) || (seg.flags & SYN)){
			freeb(bp);
			reset(source, dest, tos, length, &seg);
			goto done;
		}

		if(!(seg.flags & ACK)) {
			freeb(bp);	
			goto done;
		}

		switch(tcb->state) {
		case Syn_received:
			if(seq_within(seg.ack, tcb->snd.una+1, tcb->snd.nxt)){
				update(s, &seg);
				setstate(s, Established);
			}
			else {
				freeb(bp);
				reset(source, dest, tos, length, &seg);
				goto done;
			}
			break;
		case Established:
		case Close_wait:
			update(s, &seg);
			break;
		case Finwait1:
			update(s, &seg);
			if(tcb->sndcnt == 0)
				setstate(s, Finwait2);
			break;
		case Finwait2:
			update(s, &seg);
			break;
		case Closing:
			update(s, &seg);
			if(tcb->sndcnt == 0) {
				setstate(s, Time_wait);
				tcb->timer.start = MSL2 * (1000 / MSPTICK);
				start_timer(&tcb->timer);
			}
			break;
		case Last_ack:
			update(s, &seg);
			if(tcb->sndcnt == 0) {
				close_self(s, Enoerror);
				goto done;
			}			
		case Time_wait:
			tcb->flags |= FORCE;
			start_timer(&tcb->timer);
		}

		if((seg.flags&URG) && seg.up) {
			if(seq_gt(seg.up + seg.seq, tcb->rcv.up)) {
				tcb->rcv.up = seg.up + seg.seq;
				pullb(&bp, seg.up);
			}
		} 
		else if(seq_gt(tcb->rcv.nxt, tcb->rcv.up))
			tcb->rcv.up = tcb->rcv.nxt;

		if(length != 0) {
			switch(tcb->state){
			default:
				/* Ignore segment text */
				freeb(bp);
				break;

			case Syn_received:
			case Established:
			case Finwait1:
			case Finwait2:
				/* Place on receive queue */
				tcb->rcvcnt += blen(bp);
				if(bp)
				if(s->readq) {
					PUTNEXT(s->readq, bp);
					bp = 0;
				}
				tcb->rcv.nxt += length;

				tcprcvwin(s);
	
				start_timer(&tcb->acktimer);

				if(tcb->max_snd <= tcb->rcv.nxt-tcb->last_ack)
					tcb->flags |= FORCE;
				break;
			}
		}

		if(seg.flags & FIN) {
			tcb->flags |= FORCE;

			switch(tcb->state) {
			case Syn_received:
			case Established:
				tcb->rcv.nxt++;
				setstate(s, Close_wait);
				break;
			case Finwait1:
				tcb->rcv.nxt++;
				if(tcb->sndcnt == 0) {
					setstate(s, Time_wait);
					tcb->timer.start = MSL2 * (1000/MSPTICK);
					start_timer(&tcb->timer);
				}
				else 
					setstate(s, Closing);
				break;
			case Finwait2:
				tcb->rcv.nxt++;
				setstate(s, Time_wait);
				tcb->timer.start = MSL2 * (1000/MSPTICK);
				start_timer(&tcb->timer);
				break;
			case Close_wait:
			case Closing:
			case Last_ack:
				break;
			case Time_wait:
				start_timer(&tcb->timer);
				break;
			}
		}
		while(tcb->reseq != 0) {
			if(seq_ge(tcb->rcv.nxt, tcb->reseq->seg.seq) == 0)
				break;
			get_reseq(tcb, &tos, &seg, &bp, &length);
			if(trim(tcb, &seg, &bp, &length) == 0)
				goto gotone;
		}
		break;
		gotone:;
	}
output:
	tcp_output(s);
done:
	qunlock(tcb);
}

void
update(Ipconv *s, Tcp *seg)
{
	ushort acked;
	ushort expand;
	Tcpctl *tcb = &s->tcpctl;
	int rtt;
	int abserr;

	if(seq_gt(seg->ack, tcb->snd.nxt)) {
		tcb->flags |= FORCE;
		return;
	}

	if(seq_ge(seg->ack,tcb->snd.wl2))
	if(seq_gt(seg->seq,tcb->snd.wl1) || (seg->seq == tcb->snd.wl1)) {
		if(seg->wnd != 0)
		if(tcb->snd.wnd == 0)
			tcb->snd.ptr = tcb->snd.una;

		tcb->snd.wnd = seg->wnd;
		tcb->snd.wl1 = seg->seq;
		tcb->snd.wl2 = seg->ack;
	}

	if(!seq_gt(seg->ack, tcb->snd.una))
		return;	

	acked = seg->ack - tcb->snd.una;
	if(tcb->cwind < tcb->snd.wnd) {
		if(tcb->cwind < tcb->ssthresh)
			expand = MIN(acked,tcb->mss);
		else
			expand = ((long)tcb->mss * tcb->mss) / tcb->cwind;

		if(tcb->cwind + expand < tcb->cwind)
			expand = 65535 - tcb->cwind;
		if(tcb->cwind + expand > tcb->snd.wnd)
			expand = tcb->snd.wnd - tcb->cwind;
		if(expand != 0)
			tcb->cwind += expand;
	}

	/* Round trip time estimation */
	if(run_timer(&tcb->rtt_timer))
	if(seq_ge(seg->ack, tcb->rttseq)) {
		stop_timer(&tcb->rtt_timer);
		if((tcb->flags&RETRAN) == 0) {
			rtt = tcb->rtt_timer.start - tcb->rtt_timer.count;
			rtt *= MSPTICK;	
			if(rtt > tcb->srtt &&
			  (tcb->state == Syn_sent || tcb->state == Syn_received))
				tcb->srtt = rtt;
			else {
				if(rtt > tcb->srtt)
					abserr = rtt - tcb->srtt;
				else
					abserr = tcb->srtt - rtt;
				tcb->srtt = ((AGAIN-1)*tcb->srtt + rtt) / AGAIN;
				tcb->mdev = ((DGAIN-1)*tcb->mdev + abserr) / DGAIN;
				DPRINT("tcpout: rtt %d, srtt %d, mdev %d\n", 
					rtt, tcb->srtt, tcb->mdev);
			}
			tcb->backoff = 0;
		}
	}

	/* If we're waiting for an ack of our SYN, note it and adjust count */
	if((tcb->flags & SYNACK) == 0){
		tcb->flags |= SYNACK;
		acked--;
		tcb->sndcnt--;
	}

	pullb(&tcb->sndq, acked);

	tcb->sndcnt -= acked;
	tcb->snd.una = seg->ack;
	if (seq_gt(seg->ack, tcb->snd.up))
		tcb->snd.up = seg->ack;

	/* Stop retransmission timer, but restart it if there is still
	 * unacknowledged data.
	 */	
	stop_timer(&tcb->timer);
	if(tcb->snd.una != tcb->snd.nxt)
		start_timer(&tcb->timer);

	if(seq_lt(tcb->snd.ptr, tcb->snd.una))
		tcb->snd.ptr = tcb->snd.una;

	/* All data is acked now */
	tcb->flags &= ~RETRAN;
	tcb->backoff = 0;
}

int
in_window(Tcpctl *tcb, int seq)
{
	return seq_within(seq, tcb->rcv.nxt, (int)(tcb->rcv.nxt+tcb->rcv.wnd-1));
}

void
proc_syn(Ipconv *s, char tos, Tcp *seg)
{
	Tcpctl *tcb = &s->tcpctl;
	ushort mtu;


	tcb->flags |= FORCE;

	if(PREC(tos) > PREC(tcb->tos))
		tcb->tos = tos;

	tcb->rcv.up = tcb->rcv.nxt = seg->seq + 1;
	tcb->snd.wl1 = tcb->irs = seg->seq;
	tcb->snd.wnd = seg->wnd;

	if(seg->mss != 0)
		tcb->mss = seg->mss;

	tcb->max_snd = seg->wnd;
	if((mtu = s->ipinterface->maxmtu) != 0) {
		mtu -= TCP_HDRSIZE + TCP_EHSIZE + TCP_PHDRSIZE; 
		tcb->cwind = tcb->mss = MIN(mtu, tcb->mss);
	}
}

/* Generate an initial sequence number and put a SYN on the send queue */
void
send_syn(Tcpctl *tcb)
{
	static int start;

	start += 250000;
	tcb->iss = start;
	tcb->rttseq = tcb->iss;
	tcb->snd.wl2 = tcb->iss;
	tcb->snd.una = tcb->iss;
	tcb->snd.ptr = tcb->snd.nxt = tcb->rttseq;
	tcb->sndcnt++;
	tcb->flags |= FORCE;
}

void
add_reseq(Tcpctl *tcb, char tos, Tcp *seg, Block *bp, ushort length)
{
	Reseq *rp, *rp1;

	qlock(&reseqlock);
	if(!reseqfree) {
		qunlock(&reseqlock);
		print("tcp: no resequence descriptors\n");
		freeb(bp);
		return;
	}

	rp = reseqfree;
	reseqfree = rp->next;
	qunlock(&reseqlock);

	rp->seg = *seg;
	rp->tos = tos;
	rp->bp = bp;
	rp->length = length;

	/* Place on reassembly list sorting by starting seq number */
	rp1 = tcb->reseq;
	if(rp1 == 0 || seq_lt(seg->seq, rp1->seg.seq)) {
		rp->next = rp1;
		tcb->reseq = rp;
	} 
	else {
		for(;;){
			if(rp1->next == 0 || seq_lt(seg->seq, rp1->next->seg.seq)) {
				rp->next = rp1->next;
				rp1->next = rp;
				break;
			}
			rp1 = rp1->next;
		}
	}
}

void
get_reseq(Tcpctl *tcb, char *tos, Tcp *seg, Block **bp, ushort *length)
{
	Reseq *rp;

	if((rp = tcb->reseq) == 0)
		return;

	tcb->reseq = rp->next;

	*tos = rp->tos;
	*seg = rp->seg;
	*bp = rp->bp;
	*length = rp->length;

	qlock(&reseqlock);
	rp->next = reseqfree;
	reseqfree = rp;
	qunlock(&reseqlock);
}

int
trim(Tcpctl *tcb, Tcp *seg, Block **bp, ushort *length)
{
	Block *nbp;
	long dupcnt;
	long excess;
	ushort len;
	char accept;

	accept = 0;
	len = *length;
	if(seg->flags & SYN)
		len++;
	if(seg->flags & FIN)
		len++;

	if(tcb->rcv.wnd == 0) {
		if(len == 0)
		if(seg->seq == tcb->rcv.nxt)
			return 0;
	}
	else {
		/* Some part of the segment must be in the window */
		if(in_window(tcb,seg->seq)) {
			accept++;
		}
		else if(len != 0) {
			if(in_window(tcb, (int)(seg->seq+len-1)) || 
			seq_within(tcb->rcv.nxt, seg->seq,(int)(seg->seq+len-1)))
				accept++;
		}
	}
	if(!accept) {
		freeb(*bp);
		return -1;
	}
	dupcnt = tcb->rcv.nxt - seg->seq;
	if(dupcnt > 0){
		tcb->rerecv += dupcnt;
		if(seg->flags & SYN){
			seg->flags &= ~SYN;
			seg->seq++;

			if (seg->up > 1)
				seg->up--;
			else
				seg->flags &= ~URG;
			dupcnt--;
		}
		if(dupcnt > 0){
			pullb(bp, (ushort)dupcnt);
			seg->seq += dupcnt;
			*length -= dupcnt;

			if (seg->up > dupcnt)
				seg->up -= dupcnt;
			else {
				seg->flags &= ~URG;
				seg->up = 0;
			}
		}
	}
	excess = seg->seq + *length - (tcb->rcv.nxt + tcb->rcv.wnd);
	if(excess > 0) {
		tcb->rerecv += excess;
		*length -= excess;
		nbp = copyb(*bp, *length);
		freeb(*bp);
		*bp = nbp;
		seg->flags &= ~FIN;
	}
	return 0;
}

int
pullb(Block **bph, int count)
{
	int n, bytes;
	Block *bp;

	bytes = 0;
	if(bph == 0)
		return 0;

	while(*bph && count != 0) {
		bp = *bph;
		n = MIN(count, BLEN(bp));
		bytes += n;
		count -= n;
		bp->rptr += n;
		if(BLEN(bp) == 0) {
			*bph = bp->next;
			bp->next = 0;
			freeb(bp);
		}
	}
	return bytes;
}

int
dupb(Block **hp, Block *bp, int offset, int count)
{
	int i, blen, bytes = 0;
	uchar *addr;
	
	*hp = allocb(count);
	if(*hp == 0)
		return 0;

	/* Correct to front of data area */
	while(bp && offset && offset >= BLEN(bp)) {
		offset -= BLEN(bp);
		bp = bp->next;
	}
	if(bp == 0)
		return 0;

	addr = bp->rptr + offset;
	blen = BLEN(bp) - offset;

	while(count) {
		i = MIN(count, blen);
		memmove((*hp)->wptr, addr, i);
		(*hp)->wptr += i;
		bytes += i;
		count -= i;
		bp = bp->next;
		if(!bp)
			break;
		blen = BLEN(bp);
		addr = bp->rptr;
	}

	return bytes;
}

ushort tcp_mss = DEF_MSS;	/* Maximum segment size to be sent with SYN */
int tcp_irtt = DEF_RTT;		/* Initial guess at round trip time */

static void
cleartcp(struct Tctl *a)
{
	memset(a, 0, sizeof(struct Tctl));
}

void
init_tcpctl(Ipconv *s)
{

	Tcpctl *tcb = &s->tcpctl;

	cleartcp(tcb);

	tcb->cwind = tcb->mss = tcp_mss;
	tcb->ssthresh = 65535;
	tcb->srtt = tcp_irtt;

	/* Initialize timer intervals */
	tcb->timer.start = tcb->srtt / MSPTICK;
	tcb->timer.func = (void(*)(void*))tcp_timeout;
	tcb->timer.arg = (void *)s;
	tcb->rtt_timer.start = MAX_TIME; 

	/* Initialise ack timer */
	tcb->acktimer.start = TCP_ACK / MSPTICK;
	tcb->acktimer.func = (void(*)(void*))tcp_acktimer;
	tcb->acktimer.arg = (void *)s;
}

void
close_self(Ipconv *s, char reason[])
{
	Reseq *rp,*rp1;
	Tcpctl *tcb = &s->tcpctl;

	stop_timer(&tcb->timer);
	stop_timer(&tcb->rtt_timer);
	s->err = reason;

	/* Flush reassembly queue; nothing more can arrive */
	for(rp = tcb->reseq;rp != 0;rp = rp1){
		rp1 = rp->next;
		freeb(rp->bp);

		qlock(&reseqlock);
		rp->next = reseqfree;
		reseqfree = rp;
		qunlock(&reseqlock);
	}

	tcb->reseq = 0;
	s->err = reason;
	setstate(s, Closed);
}

int
seq_within(int x, int low, int high)
{
	if(low <= high){
		if(low <= x && x <= high)
			return 1;
	}
	else {
		if(low >= x && x >= high)
			return 1;
	}
	return 0;
}

int
seq_lt(int x, int y)
{
	return (long)(x-y) < 0;
}

int
seq_le(int x, int y)
{
	return (long)(x-y) <= 0;
}

int
seq_gt(int x, int y)
{
	return (long)(x-y) > 0;
}

int
seq_ge(int x, int y)
{
	return (long)(x-y) >= 0;
}

void
setstate(Ipconv *s, char newstate)
{
	char oldstate;
	Tcpctl *tcb = &s->tcpctl;

	oldstate = tcb->state;
	tcb->state = newstate;
	state_upcall(s, oldstate, newstate);
}

Block *
htontcp(Tcp *tcph, Block *data, Tcphdr *ph)
{
	ushort hdrlen;
	int dlen;
	ushort csum;
	Tcphdr *h;
	Block *bp;

	hdrlen = TCP_HDRSIZE;
	if(tcph->mss)
		hdrlen += MSS_LENGTH;

	if(data) {
		dlen = blen(data);	
		data = padb(data, hdrlen + TCP_PKT);
		if(data == 0)
			return 0;
		/* If we collected blocks delimit the end of the chain */
		for(bp = data; bp->next; bp = bp->next)
			bp->flags &= ~S_DELIM;
		bp->flags |= S_DELIM;
	}
	else {
		dlen = 0;
		data = allocb(hdrlen + TCP_PKT);
		if(data == 0)
			return 0;
		data->wptr += hdrlen + TCP_PKT;
		data->flags |= S_DELIM;
	}


	memmove(data->rptr, ph, TCP_PKT);
	
	h = (Tcphdr *)(data->rptr);
	h->proto = IP_TCPPROTO;
	h->frag[0] = 0;
	h->frag[1] = 0;
	hnputs(h->tcplen, hdrlen + dlen);
	hnputs(h->tcpsport, tcph->source);
	hnputs(h->tcpdport, tcph->dest);
	hnputl(h->tcpseq, tcph->seq);
	hnputl(h->tcpack, tcph->ack);
	hnputs(h->tcpflag, (hdrlen<<10) | tcph->flags);
	hnputs(h->tcpwin, tcph->wnd);
	h->tcpcksum[0] = 0;
	h->tcpcksum[1] = 0;
	h->Unused = 0;
	hnputs(h->tcpurg, tcph->up);

	if(tcph->mss != 0){
		h->tcpopt[0] = MSS_KIND;
		h->tcpopt[1] = MSS_LENGTH;
		hnputs(h->tcpmss, tcph->mss);
	}
	csum = ptcl_csum(data, TCP_EHSIZE+TCP_IPLEN, hdrlen+dlen+TCP_PHDRSIZE);
	hnputs(h->tcpcksum, csum);

	return data;
}

int
ntohtcp(Tcp *tcph, Block **bpp)
{
	ushort hdrlen;
	ushort i, optlen;
	Block *nbp;
	Tcphdr *h;
	uchar *optr;

	*bpp = pullup(*bpp, TCP_PKT+TCP_HDRSIZE);
	if(*bpp == 0)
		return -1;

	h = (Tcphdr *)((*bpp)->rptr);
	tcph->source = nhgets(h->tcpsport);
	tcph->dest = nhgets(h->tcpdport);
	tcph->seq = nhgetl(h->tcpseq);
	tcph->ack = nhgetl(h->tcpack);

	hdrlen = (h->tcpflag[0] & 0xf0) >> 2;
	if(hdrlen < TCP_HDRSIZE) {
		freeb(*bpp);
		return -1;
	}

	tcph->flags = h->tcpflag[1];
	tcph->wnd = nhgets(h->tcpwin);
	tcph->up = nhgets(h->tcpurg);
	tcph->mss = 0;

	*bpp = pullup(*bpp, hdrlen+TCP_PKT);
	if(!*bpp)
		return -1;

	for(optr = h->tcpopt, i = TCP_HDRSIZE; i < hdrlen;) {
		switch(*optr++){
		case EOL_KIND:
			return hdrlen;
		case NOOP_KIND:
			i++;
			break;
		case MSS_KIND:
			optlen = *optr++;
			if(optlen == MSS_LENGTH)
				tcph->mss = nhgets(optr);
			i += optlen;
			break;
		}
	}
	return hdrlen;
}
