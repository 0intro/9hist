#include "syslibc.h"
#include "lock.h"
#include "chan.h"
#include "proc.h"
#include "user.h"
#include "errno.h"
#include "lint.h"
#include "mem.h"
#include "mempool.h"
#include "stream.h"
#include "dkparam.h"
#include "misc.h"

enum {
	Nurp=		32,
	MSrexmit=	1000,
};

typedef struct Urp	Urp;

/*
 * URP status
 */
struct urpstat {
	ulong	input;		/* bytes read from urp */
	ulong	output;		/* bytes output to urp */
	ulong	rxmit;		/* retransmit rejected urp msg */
	ulong	rjtrs;		/* reject, trailer size */
	ulong	rjpks;		/* reject, packet size */
	ulong	rjseq;		/* reject, sequence number */
	ulong	levelb;		/* unknown level b */
	ulong	enqsx;		/* enqs sent */
	ulong	enqsr;		/* enqs rcved */
} urpstat;

struct Urp {
	Qlock;
	short	state;		/* flags */

	/* input */

	ulong	iwindow;	/* input window */
	uchar	iseq;		/* last good input sequence number */
	uchar	lastecho;	/* last echo/rej sent */
	uchar	trbuf[3];	/* trailer being collected */
	short	trx;		/* # bytes in trailer being collected */

	/* output */

	Queue	*rq;
	Queue	*wq;
	int	XW;		/* blocks per xmit window */
	int	maxblock;	/* max block size */
	int	timeout;	/* a timeout has occurred */
	int	WS;		/* start of current window */
	int	WACK;		/* first non-acknowledged message */
	int	WNX;		/* next message to be sent */
	Rendez	r;		/* process waiting in urpoput */
	Rendez	kr;		/* process waiting in urpoput */
	Block	*xb[8];		/* the xmit window buffer */
	uchar	timer;		/* timeout for xmit */
};
#define WINDOW(u) ((u->WS + u->XW - u->WNX)%8)
#define BETWEEN(x, s, n) (s<=n ? x>=s && x<n : x<n || x>=s)
#define UNACKED(x, u) (BETWEEN(x, u->WACK, u->WNX)

/*
 *  Protocol control bytes
 */
#define	SEQ	0010		/* sequence number, ends trailers */
#undef	ECHO
#define	ECHO	0020		/* echos, data given to next queue */
#define	REJ	0030		/* rejections, transmission error */
#define	ACK	0040		/* acknowledgments */
#define	BOT	0050		/* beginning of trailer */
#define	BOTM	0051		/* beginning of trailer, more data follows */
#define	BOTS	0052		/* seq update algorithm on this trailer */
#define	SOU	0053		/* start of unsequenced trailer */
#define	EOU	0054		/* end of unsequenced trailer */
#define	ENQ	0055		/* xmitter requests flow/error status */
#define	CHECK	0056		/* xmitter requests error status */
#define	INITREQ 0057		/* request initialization */
#define	INIT0	0060		/* disable trailer processing */
#define	INIT1	0061		/* enable trailer procesing */
#define	AINIT	0062		/* response to INIT0/INIT1 */
#undef	DELAY
#define	DELAY	0100		/* real-time printing delay */
#define	BREAK	0110		/* Send/receive break (new style) */

#define	REXMITING	0001
#define	REXMIT		0002
#define	INITING 	0004

Urp	urp[Nurp];

/*
 *  predeclared
 */
static void	urpiput(Queue*, Block*, Rendez*);
static void	urpoput(Queue*, Block*, Rendez*);
static void	urpopen(Queue*, Stream*);
static void	urpclose(Queue *);
static void	rcvack(Urp*, int);
static void	flushinput(Urp*);
static void	sendctl(Queue*, int);
static void	queuectl(Urp*, int);
static void	initoutput(Urp*, int);
static void	initinput(Urp*, int);

Qinfo urpinfo = { urpiput, urpoput, urpopen, urpclose, "urp" };

int
urpopen(Queue *q, Stream *s)
{
	Urp *up;
	int i;

	/*
	 *  find a free urp structure
	 */
	for(up = urp; up < &urp[Nurp]; up++){
		qlock(up);
		if(up->state == 0)
			break;
		qunlock(up);
	}
	if(up == &urp[Nurp])
		error(0, Egreg);

	q->ptr = = q->other->ptr = up;
	up->rq = q;
	up->wq = WR(q);
	q->put = urpciput;
	qunlock(up);
	initinput(up, 0);
	initoutput(up, 0);
}

/*
 *  Shut it down.
 */
static int
allacked(void *a)
{
	Urp *up;

	up = (Urp *)a;
	return up->WACK == up->WNX;
}
urpclose(Queue *q)
{
	Block *bp;
	Urp *up;
	int i;

	up = (Urp *)q->ptr;
	up->state |= LCLOSE;

	/*
	 *  wait for output to get acked
	 */
	while(!urpdone(up))
		sleep(&up->r, urpdone, up);
	up->state = 0;
}

/*
 *  upstream control messages
 */
static void
urpctliput(Urp *up, Queue *q, Block *bp)
{
	switch(bp->type){
	case M_HANGUP:
		/*
		 *  ack all outstanding messages
		 */
		lock(up);
		up->state &= ~(INITING);
		up->state |= RCLOSE;
		unlock(up);
		if(up->WS<up->WNX)
			urprack(up, ECHO+((up->WNX-1)&07));
	}
	PUTNEXT(q, bp);
}

/*
 *  character mode input.  the last character in EVERY block is
 *  a control character.
 */
void
urpciput(Queue *q, Block *bp, Rendez *rp)
{
	Urp *up;
	int i, full;
	Block *nbp;

	up = (Urp *)q->ptr;
	if(bp->type != M_DATA){
		urpctliput(up, q, bp);
		return;
	}

	/*
	 *  get the control character
	 */
	if(bp->wptr > bp->rptr)
		ctl = *(--bp->wptr);
	else
		ctl = 0;

	/*
	 *  send the block upstream
	 */
	if(bp->wptr > bp->rptr)
		PUTNEXT(q, bp);
	else
		freeb(bp);

	/*
	 *  handle the control character
	 */
	switch(ctl){
	case 0:
		break;

	case ENQ:
		urpstat.enqsr++;
		queuectl(up, up->lastecho);
		queuectl(up, ACK+up->iseq);
		flushinput(up);
		break;

	case CHECK:
		queuectl(up, ACK+up->iseq);
		break;

	case AINIT:
		up->state &= ~INITING;
		flushinput(up);
		wakeup(&cp->kr);
		break;

	case INIT0:
	case INIT1:
		queuectl(up, AINIT);
		if(*bp->rptr == INIT1)
			q->put = urpbiput;
		initinput(up, 0);
		break;

	case INITREQ:
		initoutput(up, 0);
		break;

	case BREAK:
		break;

	case ACK+0: case ACK+1: case ACK+2: case ACK+3:
	case ACK+4: case ACK+5: case ACK+6: case ACK+7:
	case ECHO+0: case ECHO+1: case ECHO+2: case ECHO+3:
	case ECHO+4: case ECHO+5: case ECHO+6: case ECHO+7:
		rcvack(up, ctl);
		break;

	case SEQ+0: case SEQ+1: case SEQ+2: case SEQ+3:
	case SEQ+4: case SEQ+5: case SEQ+6: case SEQ+7:
		/*
		 *  acknowledge receipt
		 */
		ctl = ctl & 07;
		if(q->next->len < Streamhi){
			queuectl(up, ECHO+ctl);
			up->lastecho = ECHO+ctl;
			wakeup(&cp->kr);
		}
		up->iseq = ctl;
		break;
	}
}

/*
 *  block mode input.  the last character in EVERY block is a control character.
 */
void
urpbiput(Queue *q, Block *bp, Rendez *rp)
{
	Urp *up;
	int i, full;
	Block *nbp;

	up = (Urp *)q->ptr;
	if(bp->type != M_DATA){
		urpctliput(up, q, bp);
		return;
	}

	/*
	 *  get the control character
	 */
	if(bp->wptr > bp->rptr)
		ctl = *(--bp->wptr);
	else
		ctl = 0;

	/*
	 *  take care of any block count(trx)
	 */
	while(bp->wptr > bp->rptr && up->trx){
		switch (up->trx) {
		case 1:
		case 2:
			up->trbuf[up->trx++] = *bp->rptr++;
			continue;
		default:
			up->trx = 0;
		case 0:
			break;
		}
	}

	/*
	 *  queue the block
	 */
	if(bp->wptr > bp->rptr){
		if(q->len > up->iwindow){
			flushinput(up);
			freeb(bp);
			return;
		}
		putq(q, bp);
	} else
		freeb(bp);

	/*
	 *  handle the control character
	 */
	switch(ctl){
	case 0:
		break;
	case ENQ:
		urpstat.enqsr++;
		queuectl(up, up->lastecho);
		queuectl(up, ACK+up->iseq);
		flushinput(up);
		break;

	case CHECK:
		queuectl(WR(q)->next, ACK+up->iseq);
		break;

	case AINIT:
		up->state &= ~INITING;
		flushinput(up);
		wakeup(&cp->kr);
		break;

	case INIT0:
	case INIT1:
		queuectl(up, AINIT);
		if(*bp->rptr == INIT0)
			q->put = urpciput;
		initinput(up, 0);
		break;

	case INITREQ:
		initoutput(up, 0);
		break;

	case BREAK:
		break;

	case BOT:
	case BOTS:
	case BOTM:
		up->trx = 1;
		up->trbuf[0] = ctl;
		break;

	case REJ+0: case REJ+1: case REJ+2: case REJ+3:
	case REJ+4: case REJ+5: case REJ+6: case REJ+7:
		rcvack(up, ctl);
		break;
	
	case ACK+0: case ACK+1: case ACK+2: case ACK+3:
	case ACK+4: case ACK+5: case ACK+6: case ACK+7:
	case ECHO+0: case ECHO+1: case ECHO+2: case ECHO+3:
	case ECHO+4: case ECHO+5: case ECHO+6: case ECHO+7:
		rcvack(up, ctl);
		break;

	/*
	 *  this case is extremely ugliferous
	 */
	case SEQ+0: case SEQ+1: case SEQ+2: case SEQ+3:
	case SEQ+4: case SEQ+5: case SEQ+6: case SEQ+7:
		i = ctl & 07;
		if(up->trx != 3){
			urpstat.rjtrs++;
			flushinput(up);
			break;
		} else if(q->len != up->trbuf[1] + (up->trbuf[2]<<8)){
			urpstat.rjpks++;
			flushinput(up);
			break;
		} else if(i != ((up->iseq+1)&07))) {
			urpstat.rjseq++;
			flushinput(up);
			break;
		}

		/*
		 *  send data upstream
		 */
		if(q->first) {
			q->first->flags |= S_DELIM;
			while(bp = getq(q))
				PUTNEXT(q, nbp);
		} else {
			bp = allocb(0);
			bp->flag |= S_DELIM;
			PUTNEXT(q, bp);
		{
		up->trx = 0;

		/*
		 *  acknowledge receipt
		 */
		ctl = ctl & 07;
		if(q->next->len < Streamhi){
			queuectl(up, ECHO+ctl);
			up->lastecho = ECHO+ctl;
			wakeup(&cp->kr);
		}
		up->iseq = ctl;
		break;
	}
}

/*
 *  downstream control
 */
static void
urpctloput(Urp *up, Queue *q, Block *bp)
{
	int fields[2];
	int n;
	int inwin=0, outwin=0;

	switch(bp->type){
	case M_CTL:
		if(streamparse("init", bp)){
			switch(getfields(bp->rptr, fields, 2, ' ')){
			case 2:
				inwin = strtoul(fields[1], 0, 0);
			case 1:
				outwin = strtoul(fields[0], 0, 0);
			}
			initinput(up, inwin);
			initoutput(up, outwin);
			freeb(bp);
			return;
		}
	}
	PUTNEXT(q, bp);
}

/*
 * accept data from writer
 */
urpoput(Queue *q, Block *bp)
{
	Urp *up;

	up = (Urp *)q->ptr;

	if(bp->type != M_DATA){
		urpctloput(up, q, bp);
		return;
	}

	urpstat.output + =  bp->wptr - bp->rptr;

	for(;;){
	}
}

/*
 * wait for an AINIT
 */
void
urpopenwait(Urp *up, Queue *q)
{
	while((up->state&ISOPENING) && !(up->state&RCLOSE)) {
		proc->state = Waiting;
		up->proc = proc;
		putctl1(q->next, M_RDATA, INIT1);
		if((up->state&ISOPENING) == 0){
			up->proc = 0;
			if(proc->state == Waiting){
				proc->state = Running;
				return;
			}
		}
		sched();
		up->proc = 0;
	}
}

/*
 *  wait till transmission is complete
 */
void
urpxmitwait(Urp *up, Queue *q)
{
	Block *bp;
	int debug;

	debug = (q->flag&QDEBUG);

	up->timeout = 0;
	while(!EMPTY(q) || up->WS<up->WNX){
		/*
		 *  clean up if the channel closed
		 */
		if (up->state & RCLOSE) {
			while(bp = getq(q))
				freeb(bp);
			up->WACK = up->WS = up->WNX;
		}
		/*
		 * free acked blocks
		 */
		urpfreeacked(up);
		/*
		 * retransmit if requested
		 */
		if(up->state&REXMIT)
			urprexmit(up, q);
		/*
		 * fill up the window
		 */
		if(!EMPTY(q) && WINDOW(up))
			urpfillwindow(up, q);
		/*
		 * ask other end for its status
		 */
		if(up->timeout){
			urpstat.enqsx++;
			putctl1(q->next, M_RDATA, ENQ);
			up->timeout = 0;
		}
		lock(up->olock);
		if((up->state&RCLOSE) == 0 && up->WS! = up->WNX) {
			proc->state = Waiting;
			up->proc = proc;
			unlock(up->olock);
			sched();
		} else
			unlock(up->olock);
	}
	urpfreeacked(up);
	up->WS = up->WACK = up->WF = up->WNX = up->WNX&07;
}

/*
 * fill up the urp output window
 */
void
urpfillwindow(Urp *up, Queue *q)
{
	Block *bp, *xbp;
	int debug;

	debug = (q->flag&QDEBUG);

	/*
	 * now process any thing that fits in the flow control window
	 */
	while (WINDOW(up)) {
		bp = getq(q);
		if (bp  ==  NULL)
			break;
		/* force MAXBLOCK length segments */
		if (bp->rptr+up->maxblock < bp->wptr) {
			xbp = allocb(0);
			if (xbp == NULL) {
				putbq(q, bp);
				break;
			}
			xbp->rptr = bp->rptr;
			bp->rptr + =  up->maxblock;
			xbp->wptr = bp->rptr;
			putbq(q, bp);
			bp = xbp;
		}
		/*
		 *  put new block in the block array.  if something is already
		 *  there, it should be because we haven't gotten around to freeing
		 *  it yet.  If not, complain.
		 */
		if (up->xb[up->WNX&07]) {
			urpfreeacked(up);
			if (up->xb[up->WNX&07]) {
				uprint(up, "urpfillwindow: overlap");
				freeb(up->xb[up->WNX&07]);
			}
		}
		up->xb[up->WNX&07] = bp;
		up->WNX++;
		urpxmit(q, bp, up->WNX-1);
	}
}

/*
 *  Send out a message, with trailer.
 */
void
urpxmit(Queue *q, Block *bp, int seqno)
{
	Urp *up = (Urp *)q->ptr;
	int size;
	Block *xbp;
	int debug;

	if (bp == NULL) {
		print("null bp in urpxmit\n");
		return;
	}
	debug = (q->flag&QDEBUG);
	size = bp->wptr - bp->rptr;
	seqno & =  07;
	/* send ptr to block, if non-empty */
	if (size) {
		if ((xbp = allocb(0))  ==  NULL){
			print("can't xmit\n");
			return;
		}
		xbp->rptr = bp->rptr;
		xbp->wptr = bp->wptr;
		xbp->type = bp->type;
		PUTNEXT(q, xbp);
	}
	/* send trailer */
	if ((xbp = allocb(3))  ==  NULL){
		print("can't xmit2\n");
		return;
	}
	xbp->type = M_RDATA;
	*xbp->wptr++ = (bp->class&S_DELIM) ? BOT: BOTM;
	*xbp->wptr++ = size;
	*xbp->wptr++ = size >> 8;
	PUTNEXT(q, xbp);
	putctl1(q->next, M_RDATA, SEQ + seqno);
	up->timer = DKPTIME;
}

void
urprexmit(Urp *up, Queue *q)
{
	int i;

	for (i = up->WACK; i<up->WNX; i++) {
		urpxmit(q, up->xb[i&07], i);
		urpstat.rxmit++;
	}
	up->state & =  ~REXMIT;
}

/*
 *  receive an acknowledgement
 */
static void
rcvack(Urp *up, int msg)
{
	int seqno;
	int next;

	seqno = msg&07;
	next = (seqno+1) & 0x7

	lock(up);
	if(BETWEEN(seqno, up->WACK, up->WNX))
		up->WACK = next;

	switch(msg & 0370){
	case ECHO:
		up->timer = MSrexmit;	/* push off ENQ timeout */
		if(BETWEEN(seqno, up->WS, up->WNX)){
			up->WS = next;
			wakeup(&up->r);
		}
		break;
	case REJ:
	case ACK:
		if(up->WACK == next){
			up->state |= REXMIT;
			wakeup(&up->r);
		}
		break;
	}
	unlock(up);
}

/*
 * throw away any partially collected input
 */
static void
flushinput(Urp *up)
{
	Block *bp;

	while (bp = getq(up->rq))
		freeb(bp);
	up->trx = 0;
}

/*
 *  send a control character down stream
 */
static void
sendctl(Queue *q, int x)
{
	Block *bp;

	/*
	 *  send anything queued
	 */
	while(bp = getq(q))
		PUTNEXT(q, bp);

	/*
	 *  send the new byte
	 */
	bp = allocb(1);
	*bp->wptr++ = x;
	PUTNEXT(q, bp);
}

/*
 *  queue a control character to be sent down stream
 */
static void
queuectl(Urp *up, int x)
{
	Block *bp;
	Queue *q;

	q = up->wq;
	bp = allocb(1);
	*bp->wptr++ = x;
	putq(q, bp);
	wakeup(&up->r);
}

/*
 *  initialize output
 */
static void
initoutput(Urp *up, int window)
{
	/*
	 *  ack any outstanding blocks
	 */
	if(up->WS<up->WNX)
		urprack(up, ECHO+((up->WNX-1)&07));

	/*
	 *  set output window
	 */
	up->maxblock = window/4;
	if(up->maxblock < 64)
		up->maxblock = 64;
	if(up->maxblock > Streamhi/4)
		up->maxblock = Streamhi/4;
	up->XW = 4;

	/*
	 *  set sequence varialbles
	 */
	up->WS = 1;
	up->WACK = 1;
	up->WNX = 1;

	/*
	 *  tell the other side we've inited
	 */
	up->state |= ISOPENING;
	up->timer = MSrexmit;
	sendctl(q->next, INIT1);
}

/*
 *  initialize input
 */
static void
initinput(Urp *up, int window)
{
	/*
	 *  restart all sequence parameters
	 */
	up->trx = 0;
	up->iseq = 0;
	up->lastecho = ECHO+0;
	up->WF = 1;
	flushinput(up);
}
