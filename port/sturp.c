#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

enum {
	MSrexmit=	1000,
	Nmask=		0x7,
};

#define DPRINT if(q->flag&QDEBUG)kprint

typedef struct Urp	Urp;

#define NOW (MACHP(0)->ticks*MS2HZ)

/*
 * URP status
 */
struct urpstat {
	ulong	input;		/* bytes read from urp */
	ulong	output;		/* bytes output to urp */
	ulong	rexmit;		/* retransmit rejected urp msg */
	ulong	rjtrs;		/* reject, trailer size */
	ulong	rjpks;		/* reject, packet size */
	ulong	rjseq;		/* reject, sequence number */
	ulong	levelb;		/* unknown level b */
	ulong	enqsx;		/* enqs sent */
	ulong	enqsr;		/* enqs rcved */
} urpstat;

struct Urp {
	QLock;
	Urp	*list;		/* list of all urp structures */
	short	state;		/* flags */
	Rendez	r;		/* process waiting for output to finish */

	/* input */
	QLock	ack;		/* ack lock */
	Queue	*rq;		/* input queue */
	uchar	iseq;		/* last good input sequence number */
	uchar	lastecho;	/* last echo/rej sent */
	uchar	trbuf[3];	/* trailer being collected */
	short	trx;		/* # bytes in trailer being collected */
	int	blocks;

	/* output */
	QLock	xmit;		/* output lock, only one process at a time */
	Queue	*wq;		/* output queue */
	int	maxout;		/* maximum outstanding unacked blocks */
	int	maxblock;	/* max block size */
	int	next;		/* next block to send */
	int	unechoed;	/* first unechoed block */
	int	unacked;	/* first unacked block */
	int	nxb;		/* next xb to use */
	Block	*xb[8];		/* the xmit window buffer */
	QLock	xl[8];
	ulong	timer;		/* timeout for xmit */
	int	rexmit;
};

/* list of allocated urp structures (never freed) */
struct
{
	Lock;
	Urp	*urp;
} urpalloc;

Rendez	urpkr;
QLock	urpkl;
int	urpkstarted;

#define WINDOW(u) ((u)->unechoed>(u)->next ? (u)->unechoed+(u)->maxout-(u)->next-8 :\
			(u)->unechoed+(u)->maxout-(u)->next)
#define IN(x, f, n) (f<=n ? (x>=f && x<n) : (x<n || x>=f))
#define NEXT(x) (((x)+1)&Nmask)

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

#define	REJECTING	0x1
#define	INITING 	0x2
#define HUNGUP		0x4
#define	OPEN		0x8
#define CLOSING		0x10

/*
 *  predeclared
 */
static void	urpreset(void);
static void	urpciput(Queue*, Block*);
static void	urpiput(Queue*, Block*);
static void	urpoput(Queue*, Block*);
static void	urpopen(Queue*, Stream*);
static void	urpclose(Queue *);
static void	output(Urp*);
static void	sendblock(Urp*, int);
static void	rcvack(Urp*, int);
static void	flushinput(Urp*);
static void	sendctl(Urp*, int);
static void	sendack(Urp*);
static void	sendrej(Urp*);
static void	initoutput(Urp*, int);
static void	initinput(Urp*);
static void	urpkproc(void *arg);
static void	urpvomit(char*, Urp*);
static void	tryoutput(Urp*);

Qinfo urpinfo =
{
	urpciput,
	urpoput,
	urpopen,
	urpclose,
	"urp",
	urpreset
};

static void
urpreset(void)
{
}

static void
urpopen(Queue *q, Stream *s)
{
	Urp *urp;

	USED(s);
	if(!urpkstarted){
		qlock(&urpkl);
		if(!urpkstarted){
			urpkstarted = 1;
			kproc("urpkproc", urpkproc, 0);
		}
		qunlock(&urpkl);
	}

	/*
	 *  find an unused urp structure
	 */
	for(urp = urpalloc.urp; urp; urp = urp->list){
		if(urp->state == 0){
			qlock(urp);
			if(urp->state == 0)
				break;
			qunlock(urp);
		}
	}
	if(urp == 0){
		/*
		 *  none available, create a new one, they are never freed
		 */
		urp = smalloc(sizeof(Urp));
		qlock(urp);
		lock(&urpalloc);
		urp->list = urpalloc.urp;
		urpalloc.urp = urp;
		unlock(&urpalloc);
	}
	q->ptr = q->other->ptr = urp;
	q->rp = &urpkr;
	urp->rq = q;
	urp->wq = q->other;
	urp->state = OPEN;
	qunlock(urp);
	initinput(urp);
	initoutput(urp, 0);
}

/*
 *  Shut down the connection and kill off the kernel process
 */
static int
isflushed(void *a)
{
	Urp *urp;

	urp = (Urp *)a;
	return (urp->state&HUNGUP) || (urp->unechoed==urp->nxb && urp->wq->len==0);
}
static void
urpclose(Queue *q)
{
	Urp *urp;
	int i;

	urp = (Urp *)q->ptr;
	if(urp == 0)
		return;

	/*
	 *  wait for all outstanding messages to drain, tell kernel
	 *  process we're closing.
	 *
	 *  if 2 minutes elapse, give it up
	 */
	urp->state |= CLOSING;
	if(!waserror()){
		tsleep(&urp->r, isflushed, urp, 2*60*1000);
		poperror();
	}
	urp->state |= HUNGUP;

	qlock(&urp->xmit);
	/*
	 *  ack all outstanding messages
	 */
	i = urp->next - 1;
	if(i < 0)
		i = 7;
	rcvack(urp, ECHO+i);

	/*
	 *  free all staged but unsent messages
	 */
	for(i = 0; i < 7; i++)
		if(urp->xb[i]){
			freeb(urp->xb[i]);
			urp->xb[i] = 0;
		}
	qunlock(&urp->xmit);

	qlock(urp);
	urp->state = 0;
	qunlock(urp);
}

/*
 *  upstream control messages
 */
static void
urpctliput(Urp *urp, Queue *q, Block *bp)
{
	switch(bp->type){
	case M_HANGUP:
		urp->state |= HUNGUP;
		wakeup(&urp->r);
		break;
	}
	PUTNEXT(q, bp);
}

/*
 *  character mode input.
 *
 *  the first byte in every message is a ctl byte (which belongs at the end).
 */
void
urpciput(Queue *q, Block *bp)
{
	Urp *urp;
	int i;
	int ctl;

	urp = (Urp *)q->ptr;
	if(urp == 0)
		return;
	if(bp->type != M_DATA){
		urpctliput(urp, q, bp);
		return;
	}

	/*
	 *  get the control character
	 */
	ctl = *bp->rptr++;
	if(ctl < 0)
		return;

	/*
	 *  take care of any data
	 */
	if(BLEN(bp)>0  && q->next->len<2*Streamhi && q->next->nb<2*Streambhi){
		bp->flags |= S_DELIM;
		urpstat.input += BLEN(bp);
		PUTNEXT(q, bp);
	} else
		freeb(bp);

	/*
	 *  handle the control character
	 */
	switch(ctl){
	case 0:
		break;
	case ENQ:
		DPRINT("rENQ(c)\n");
		urpstat.enqsr++;
		sendctl(urp, urp->lastecho);
		sendctl(urp, ACK+urp->iseq);
		break;

	case CHECK:
		DPRINT("rCHECK(c)\n");
		sendctl(urp, ACK+urp->iseq);
		break;

	case AINIT:
		DPRINT("rAINIT(c)\n");
		urp->state &= ~INITING;
		flushinput(urp);
		tryoutput(urp);
		break;

	case INIT0:
	case INIT1:
		DPRINT("rINIT%d(c)\n", ctl-INIT0);
		sendctl(urp, AINIT);
		if(ctl == INIT1)
			q->put = urpiput;
		initinput(urp);
		break;

	case INITREQ:
		DPRINT("rINITREQ(c)\n");
		initoutput(urp, 0);
		break;

	case BREAK:
		break;

	case REJ+0: case REJ+1: case REJ+2: case REJ+3:
	case REJ+4: case REJ+5: case REJ+6: case REJ+7:
		DPRINT("rREJ%d(c)\n", ctl-REJ);
		rcvack(urp, ctl);
		break;
	
	case ACK+0: case ACK+1: case ACK+2: case ACK+3:
	case ACK+4: case ACK+5: case ACK+6: case ACK+7:
	case ECHO+0: case ECHO+1: case ECHO+2: case ECHO+3:
	case ECHO+4: case ECHO+5: case ECHO+6: case ECHO+7:
		DPRINT("%s%d(c)\n", (ctl&ECHO)?"rECHO":"rACK", ctl&7);
		rcvack(urp, ctl);
		break;

	case SEQ+0: case SEQ+1: case SEQ+2: case SEQ+3:
	case SEQ+4: case SEQ+5: case SEQ+6: case SEQ+7:
		DPRINT("rSEQ%d(c)\n", ctl-SEQ);
		qlock(&urp->ack);
		i = ctl & Nmask;
		if(!QFULL(q->next))
			sendctl(urp, urp->lastecho = ECHO+i);
		urp->iseq = i;
		qunlock(&urp->ack);
		break;
	}
}

/*
 *  block mode input.
 *
 *  the first byte in every message is a ctl byte (which belongs at the end).
 *
 *  Simplifying assumption:  one put == one message && the control byte
 *	is in the first block.  If this isn't true, strange bytes will be
 *	used as control bytes.
 *
 *	There's no input lock.  The channel could be closed while we're
 *	processing a message.
 */
void
urpiput(Queue *q, Block *bp)
{
	Urp *urp;
	int i, len;
	int ctl;

	urp = (Urp *)q->ptr;
	if(urp == 0)
		return;
	if(bp->type != M_DATA){
		urpctliput(urp, q, bp);
		return;
	}

	/*
	 *  get the control character
	 */
	ctl = *bp->rptr++;

	/*
	 *  take care of any block count(trx)
	 */
	while(urp->trx){
		if(BLEN(bp)<=0)
			break;
		switch (urp->trx) {
		case 1:
		case 2:
			urp->trbuf[urp->trx++] = *bp->rptr++;
			continue;
		default:
			urp->trx = 0;
			break;
		}
	}

	/*
	 *  queue the block(s)
	 */
	if(BLEN(bp) > 0){
		bp->flags &= ~S_DELIM;
		putq(q, bp);
		if(q->len > 4*1024){
			flushinput(urp);
			return;
		}
	} else
		freeb(bp);

	/*
	 *  handle the control character
	 */
	switch(ctl){
	case 0:
		break;
	case ENQ:
		DPRINT("rENQ %d %uo %uo\n", urp->blocks, urp->lastecho, ACK+urp->iseq);
		urp->blocks = 0;
		urpstat.enqsr++;
		sendctl(urp, urp->lastecho);
		sendctl(urp, ACK+urp->iseq);
		flushinput(urp);
		break;

	case CHECK:
		DPRINT("rCHECK\n");
		sendctl(urp, ACK+urp->iseq);
		break;

	case AINIT:
		DPRINT("rAINIT\n");
		urp->state &= ~INITING;
		flushinput(urp);
		tryoutput(urp);
		break;

	case INIT0:
	case INIT1:
		DPRINT("rINIT%d\n", ctl-INIT0);
		sendctl(urp, AINIT);
		if(ctl == INIT0)
			q->put = urpciput;
		initinput(urp);
		break;

	case INITREQ:
		DPRINT("rINITREQ\n");
		initoutput(urp, 0);
		break;

	case BREAK:
		break;

	case BOT:
	case BOTM:
	case BOTS:
		DPRINT("rBOT%c...", " MS"[ctl-BOT]);
		urp->trx = 1;
		urp->trbuf[0] = ctl;
		break;

	case REJ+0: case REJ+1: case REJ+2: case REJ+3:
	case REJ+4: case REJ+5: case REJ+6: case REJ+7:
		DPRINT("rREJ%d\n", ctl-REJ);
		rcvack(urp, ctl);
		break;
	
	case ACK+0: case ACK+1: case ACK+2: case ACK+3:
	case ACK+4: case ACK+5: case ACK+6: case ACK+7:
	case ECHO+0: case ECHO+1: case ECHO+2: case ECHO+3:
	case ECHO+4: case ECHO+5: case ECHO+6: case ECHO+7:
		DPRINT("%s%d\n", (ctl&ECHO)?"rECHO":"rACK", ctl&7);
		rcvack(urp, ctl);
		break;

	/*
	 *  if the sequence number is the next expected
	 *	and the trailer length == 3
	 *	and the block count matches the bytes received
	 *  then send the bytes upstream.
	 */
	case SEQ+0: case SEQ+1: case SEQ+2: case SEQ+3:
	case SEQ+4: case SEQ+5: case SEQ+6: case SEQ+7:
		len = urp->trbuf[1] + (urp->trbuf[2]<<8);
		DPRINT("rSEQ%d(%d,%d,%d)...", ctl-SEQ, urp->trx, len, q->len);
		i = ctl & Nmask;
		if(urp->trx != 3){
			urpstat.rjtrs++;
			sendrej(urp);
			break;
		}else if(q->len != len){
			urpstat.rjpks++;
			sendrej(urp);
			break;
		}else if(i != ((urp->iseq+1)&Nmask)){
			urpstat.rjseq++;
			sendrej(urp);
			break;
		}else if(q->next->len > (3*Streamhi)/2
			|| q->next->nb > (3*Streambhi)/2){
			DPRINT("next->len=%d, next->nb=%d\n",
				q->next->len, q->next->nb);
			flushinput(urp);
			break;
		}
		DPRINT("accept %d\n", q->len);

		/*
		 *  send data upstream
		 */
		if(q->first) {
			if(urp->trbuf[0] != BOTM)
				q->last->flags |= S_DELIM;
			while(bp = getq(q)){
				urpstat.input += BLEN(bp);
				PUTNEXT(q, bp);
			}
		} else {
			bp = allocb(0);
			if(urp->trbuf[0] != BOTM)
				bp->flags |= S_DELIM;
			PUTNEXT(q, bp);
		}
		urp->trx = 0;

		/*
		 *  acknowledge receipt
		 */
		qlock(&urp->ack);
		urp->iseq = i;
		if(!QFULL(q->next))
			sendctl(urp, urp->lastecho = ECHO|i);
		qunlock(&urp->ack);
		break;
	}
}

/*
 *  downstream control
 */
static void
urpctloput(Urp *urp, Queue *q, Block *bp)
{
	char *fields[2];
	int outwin;

	switch(bp->type){
	case M_CTL:
		if(streamparse("break", bp)){
			/*
			 *  send a break as part of the data stream
			 */
			urpstat.output++;
			bp->wptr = bp->lim;
			bp->rptr = bp->wptr - 1;
			*bp->rptr = BREAK;
			putq(q, bp);
			output(urp);
			return;
		}
		if(streamparse("init", bp)){
			outwin = strtoul((char*)bp->rptr, 0, 0);
			initoutput(urp, outwin);
			freeb(bp);
			return;
		}
		if(streamparse("debug", bp)){
			switch(getfields((char *)bp->rptr, fields, 2, ' ')){
			case 1:
				if (strcmp(fields[0], "on") == 0) {
					q->flag |= QDEBUG;
					q->other->flag |= QDEBUG;
				}
				if (strcmp(fields[0], "off") == 0) {
					q->flag &= ~QDEBUG;
					q->other->flag &= ~QDEBUG;
				}
			}
			freeb(bp);
			return;
		}
	}
	PUTNEXT(q, bp);
}

/*
 *  accept data from a writer
 */
static void
urpoput(Queue *q, Block *bp)
{
	Urp *urp;

	urp = (Urp *)q->ptr;

	if(bp->type != M_DATA){
		urpctloput(urp, q, bp);
		return;
	}

	urpstat.output += BLEN(bp);
	putq(q, bp);
	output(urp);
}

/*
 *  start output
 */
static void
output(Urp *urp)
{
	Block *bp, *nbp;
	ulong now;
	Queue *q;
	int i;

	if(!canqlock(&urp->xmit))
		return;

	if(waserror()){
		print("urp output error\n");
		qunlock(&urp->xmit);
		nexterror();
	}

	/*
	 *  if still initing and it's time to rexmit, send an INIT1
	 */
	now = NOW;
	if(urp->state & INITING){
		if(now > urp->timer){
			q = urp->wq;
			DPRINT("INITING timer (%d, %d): ", now, urp->timer);
			sendctl(urp, INIT1);
			urp->timer = now + MSrexmit;
		}
		goto out;
	}

	/*
	 *  fill the transmit buffers, `nxb' can never overtake `unechoed'
	 */
	q = urp->wq;
	i = NEXT(urp->nxb);
	if(i != urp->unechoed) {
		for(bp = getq(q); bp && i!=urp->unechoed; i = NEXT(i)){
			if(urp->xb[urp->nxb] != 0)
				urpvomit("output", urp);
			if(BLEN(bp) > urp->maxblock){
				nbp = urp->xb[urp->nxb] = allocb(0);
				nbp->rptr = bp->rptr;
				nbp->wptr = bp->rptr = bp->rptr + urp->maxblock;
			} else {
				urp->xb[urp->nxb] = bp;
				bp = getq(q);
			}
			urp->nxb = i;
		}
		if(bp)
			putbq(q, bp);
	}

	/*
	 *  retransmit cruft
	 */
	if(urp->rexmit){
		/*
		 *  if a retransmit is requested, move next back to
		 *  the unacked blocks
		 */
		urpstat.rexmit++;
		urp->rexmit = 0;
		urp->next = urp->unacked;
	} else if(urp->unechoed!=urp->next && NOW>urp->timer){
		/*
		 *  if a retransmit time has elapsed since a transmit,
		 *  send an ENQ
		 */
		DPRINT("OUTPUT timer (%d, %d): ", NOW, urp->timer);
		urp->timer = NOW + MSrexmit;
		urp->state &= ~REJECTING;
		urpstat.enqsx++;
		sendctl(urp, ENQ);
		goto out;
	}

	/*
	 *  if there's a window open, push some blocks out
	 *
	 *  the lock is to synchronize with acknowledges that free
	 *  blocks.
	 */
	while(WINDOW(urp)>0 && urp->next!=urp->nxb){
		i = urp->next;
		qlock(&urp->xl[i]);
		if(waserror()){
			qunlock(&urp->xl[i]);
			nexterror();
		}
		sendblock(urp, i);
		qunlock(&urp->xl[i]);
		urp->next = NEXT(urp->next);
		poperror();
	}
out:
	qunlock(&urp->xmit);
	poperror();
}

/*
 *  try output, this is called by an input process
 */
void
tryoutput(Urp *urp)
{
	if(!waserror()){
		output(urp);
		poperror();
	}
}

/*
 *  send a control byte, put the byte at the end of the allocated
 *  space in case a lower layer needs header room.
 */
static void
sendctl(Urp *urp, int ctl)
{
	Block *bp;
	Queue *q;

	q = urp->wq;
	if(QFULL(q->next))
		return;
	bp = allocb(1);
	bp->wptr = bp->lim;
	bp->rptr = bp->lim-1;
	*bp->rptr = ctl;
	bp->flags |= S_DELIM;
	DPRINT("sCTL %ulx\n", ctl);
	PUTNEXT(q, bp);
}

/*
 *  send a reject
 */
static void
sendrej(Urp *urp)
{
	Queue *q = urp->wq;
	flushinput(urp);
	qlock(&urp->ack);
	if((urp->lastecho&~Nmask) == ECHO){
		DPRINT("REJ %d\n", urp->iseq);
		sendctl(urp, urp->lastecho = REJ|urp->iseq);
	}
	qunlock(&urp->ack);
}

/*
 *  send an acknowledge
 */
static void
sendack(Urp *urp)
{
	/*
	 *  check the precondition for acking
	 */
	if(QFULL(urp->rq->next) || (urp->lastecho&Nmask)==urp->iseq)
		return;

	if(!canqlock(&urp->ack))
		return;

	/*
	 *  check again now that we've locked
	 */
	if(QFULL(urp->rq->next) || (urp->lastecho&Nmask)==urp->iseq){
		qunlock(&urp->ack);
		return;
	}

	/*
	 *  send the ack
	 */
	{ Queue *q = urp->wq; DPRINT("sendack: "); }
	sendctl(urp, urp->lastecho = ECHO|urp->iseq);
	qunlock(&urp->ack);
}

/*
 *  send a block.
 */
static void
sendblock(Urp *urp, int bn)
{
	int n;
	Queue *q;
	Block *bp, *m, *nbp;

	q = urp->wq;
	urp->timer = NOW + MSrexmit;
	if(QFULL(q->next))
		return;

	/*
	 *  message 1, the BOT and the data
	 */
	bp = urp->xb[bn];
	if(bp == 0)
		return;
	m = allocb(1);
	m->rptr = m->lim - 1;
	m->wptr = m->lim;
	*m->rptr = (bp->flags & S_DELIM) ? BOT : BOTM;
	nbp = allocb(0);
	nbp->rptr = bp->rptr;
	nbp->wptr = bp->wptr;
	nbp->base = bp->base;
	nbp->lim = bp->lim;
	nbp->flags |= S_DELIM;
	if(bp->type == M_CTL){
		PUTNEXT(q, nbp);
		m->flags |= S_DELIM;
		PUTNEXT(q, m);
	} else {
		m->next = nbp;
		PUTNEXT(q, m);
	}

	/*
	 *  message 2, the block length and the SEQ
	 */
	m = allocb(3);
	m->rptr = m->lim - 3;
	m->wptr = m->lim;
	n = BLEN(bp);
	m->rptr[0] = SEQ | bn;
	m->rptr[1] = n;
	m->rptr[2] = n<<8;
	m->flags |= S_DELIM;
	PUTNEXT(q, m);
	DPRINT("sb %d (%d)\n", bn, urp->timer);
}

/*
 *  receive an acknowledgement
 */
static void
rcvack(Urp *urp, int msg)
{
	int seqno;
	int next;
	int i;

	seqno = msg&Nmask;
	next = NEXT(seqno);

	/*
	 *  release any acknowledged blocks
	 */
	if(IN(seqno, urp->unacked, urp->next)){
		for(; urp->unacked != next; urp->unacked = NEXT(urp->unacked)){
			i = urp->unacked;
			qlock(&urp->xl[i]);
			if(urp->xb[i])
				freeb(urp->xb[i]);
			else
				urpvomit("rcvack", urp);
			urp->xb[i] = 0;
			qunlock(&urp->xl[i]);
		}
	}

	switch(msg & 0370){
	case ECHO:
		if(IN(seqno, urp->unechoed, urp->next)) {
			urp->unechoed = next;
		}
		/*
		 *  the next reject at the start of a window starts a 
		 *  retransmission.
		 */
		urp->state &= ~REJECTING;
		break;
	case REJ:
		if(IN(seqno, urp->unechoed, urp->next))
			urp->unechoed = next;
		/*
		 *  ... FALL THROUGH ...
		 */
	case ACK:
		/*
		 *  start a retransmission if we aren't retransmitting
		 *  and this is the start of a window.
		 */
		if(urp->unechoed==next && !(urp->state & REJECTING)){
			urp->state |= REJECTING;
			urp->rexmit = 1;
		}
		break;
	}

	tryoutput(urp);
	if(urp->state & CLOSING)
		wakeup(&urp->r);
}

/*
 * throw away any partially collected input
 */
static void
flushinput(Urp *urp)
{
	Block *bp;

	while (bp = getq(urp->rq))
		freeb(bp);
	urp->trx = 0;
}

/*
 *  initialize output
 */
static void
initoutput(Urp *urp, int window)
{
	int i;

	/*
	 *  set output window
	 */
	urp->maxblock = window/4;
	if(urp->maxblock < 64)
		urp->maxblock = 64;
	urp->maxblock -= 4;
	urp->maxout = 4;

	/*
	 *  set sequence varialbles
	 */
	urp->unechoed = 1;
	urp->unacked = 1;
	urp->next = 1;
	urp->nxb = 1;
	urp->rexmit = 0;

	/*
	 *  free any outstanding blocks
	 */
	for(i = 0; i < 8; i++){
		qlock(&urp->xl[i]);
		if(urp->xb[i])
			freeb(urp->xb[i]);
		urp->xb[i] = 0;
		qunlock(&urp->xl[i]);
	}

	/*
	 *  tell the other side we've inited
	 */
	urp->state |= INITING;
	urp->timer = NOW + MSrexmit;
	{ Queue *q = urp->wq; DPRINT("initoutput (%d): ", urp->timer); }
	sendctl(urp, INIT1);
}

/*
 *  initialize input
 */
static void
initinput(Urp *urp)
{
	/*
	 *  restart all sequence parameters
	 */
	urp->blocks = 0;
	urp->trx = 0;
	urp->iseq = 0;
	urp->lastecho = ECHO+0;
	flushinput(urp);
}

static void
urpkproc(void *arg)
{
	Urp *urp;

	USED(arg);

	if(waserror())
		;

	for(;;){
		for(urp = urpalloc.urp; urp; urp = urp->list){
			if(urp->state==0 || (urp->state&HUNGUP))
				continue;
			if(!canqlock(urp))
				continue;
			if(waserror()){
				qunlock(urp);
				continue;
			}
			if(urp->state==0 || (urp->state&HUNGUP)){
				qunlock(urp);
				poperror();
				continue;
			}
			if(urp->iseq!=(urp->lastecho&7) && !QFULL(urp->rq->next))
				sendack(urp);
			output(urp);
			qunlock(urp);
			poperror();
		}
		tsleep(&urpkr, return0, 0, 500);
	}
}

/*
 *  urp got very confused, complain
 */
static void
urpvomit(char *msg, Urp* urp)
{
	print("urpvomit: %s %ux next %d unechoed %d unacked %d nxb %d\n",
		msg, urp, urp->next, urp->unechoed, urp->unacked, urp->nxb);
	print("\txb: %ux %ux %ux %ux %ux %ux %ux %ux\n",
		urp->xb[0], urp->xb[1], urp->xb[2], urp->xb[3], urp->xb[4], 
		urp->xb[5], urp->xb[6], urp->xb[7]);
	print("\tiseq: %uo lastecho: %uo trx: %d trbuf: %uo %uo %uo\n",
		urp->iseq, urp->lastecho, urp->trx, urp->trbuf[0], urp->trbuf[1],
		urp->trbuf[2]);
	print("\tupq: %ux %d %d\n", &urp->rq->next->r,  urp->rq->next->nb,
		urp->rq->next->len);
}

void
urpfillstats(Chan *c, char *buf, int len)
{
	char b[256];

	USED(c);
	sprint(b, "in: %d\nout: %d\nrexmit: %d\nrjtrs: %d\nrjpks: %d\nrjseq: %d\nenqsx: %d\nenqsr: %d\n",
		urpstat.input, urpstat.output, urpstat.rexmit, urpstat.rjtrs,
		urpstat.rjpks, urpstat.rjseq, urpstat.enqsx, urpstat.enqsr);
	strncpy(buf, b, len);
}
