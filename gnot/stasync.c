#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#define DPRINT 	if(asyncdebug)kprint

typedef struct Async {
	QLock;

	int	inuse;
	QLock	xmit;		/* transmit lock */
	int	chan;		/* current urp channel */
	Block	*bp;		/* current buffer */
	int	count;
	ushort	crc;

	/* statistics */

	ulong	toolong;
	ulong	tooshort;
	ulong	badcrc;
	ulong	badescape;
	ulong	in;		/* bytes in */
	ulong	out;		/* bytes out */
} Async;

Async async[Nasync];

/*
 *  async stream module definition
 */
static void asynciput(Queue*, Block*);
static void asyncoput(Queue*, Block*);
static void asyncopen(Queue*, Stream*);
static void asyncclose(Queue*);
Qinfo asyncinfo = { asynciput, asyncoput, asyncopen, asyncclose, "async" };

int asyncdebug = 1;
int asyncerror;

static ushort crc_table[256] = {
#include "crc_16.h"
};

#define	BOT	0050		/* begin trailer */
#define	BOTM	0051		/* begin trailer, more data follows */
#define	BOTS	0052		/* seq update alg. on this trailer */

#define	MAXFRAME	256
#define	FRAME		0x7e
#define	STUF		0x9d

#define	CRCSTART	(crc_table[0xff])
#define	CRCFUNC(crc,x)	(crc_table[((crc)^(x))&0xff]^((crc)>>8))

/*
 *  create the kernel process for input
 */
static void
asyncopen(Queue *q, Stream *s)
{
	DPRINT("asyncstopen %d\n", s->dev);

	for(ap = async; ap < &async[Nasync]; ap++){
		qlock(ap);
		if(ap->inuse == 0)
			break;
		qunlock(ap);
	}
	if(ap == &async[Nasync])
		error(0, Enoasync);
	q->ptr = q->other->ptr = ap;

	ap->bp = allocb(256);
	ap->chan = -1;
	ap->count = 0;
	ap->toolong = 0;
	ap->tooshort = 0;
	ap->badcrc = 0;
	ap->badescape = 0;
	ap->chan0 = 0;
	ap->in = 0;
	ap->out = 0;
}

static void
asyncclose(Queue * q)
{
	Async *ap = (Async *)q->ptr;

	DPRINT("asyncstclose %d\n", ap-async);
	qlock(ap);
	ap->inuse = 0;
	qunlock(ap);
}

/*
 *  free all blocks of a message in `q', `bp' is the first block
 *  of the message
 */
static void
freemsg(Queue *q, Block *bp)
{
	for(; bp; bp = getq(q)){
		if(bp->flags & S_DELIM){
			freeb(bp);
			return;
		}
		freeb(bp);
	}
}

static void
showframe(char *t, Async *ap, uchar *buf, int n)
{
	kprint("a%d %s [", ap-async, t);
	while (--n >= 0)
		kprint(" %2.2ux", *buf++);
	kprint(" ]\n");
}

void
aswrite(Async *ap)
{
	int n = ap->bp - ap->buf;
	if (n <= 0)
		return;
	if (asyncdebug > 2)
		showframe("out", ap, ap->buf, n);
	(*devtab[ap->c->type].write)(ap->c, ap->buf, n);
	ap->out += n;
	ap->bp = ap->buf;
}

void
asputf(Async *ap, int frame)
{
	uchar *p = ap->bp;
	int c;
	if (ap->count > 0) {
		if (asyncerror)
			ap->crc^=1, asyncerror=0;
		*p++ = c = ap->crc&0xff;
		if (c == FRAME)
			*p++ = 0x00;
		*p++ = c = (ap->crc>>8)&0xff;
		if (c == FRAME)
			*p++ = 0x00;
		ap->count = 0;
	}
	if (frame) {
		*p++ = FRAME;
		*p++ = FRAME;
	}
	ap->bp = p;
	aswrite(ap);
}

void
asputc(Async *ap, int c)
{
	int d;
	uchar *p = ap->bp;
	if (ap->count <= 0) {
		*p++ = FRAME;
		*p++ = FRAME;
		*p++ = d = 0x80|((ap->chan>>5)&0x7e);
		ap->crc = CRCFUNC(CRCSTART, d);
		*p++ = d = 0x80|((ap->chan<<1)&0x7e);
		ap->crc = CRCFUNC(ap->crc, d);
	}
	*p++ = c;
	if (c == FRAME)
		*p++ = 0x00;
	ap->crc = CRCFUNC(ap->crc, c);
	ap->bp = p;
	if (++ap->count >= MAXFRAME-4)
		asputf(ap, 0);
	else if (p-ap->buf >= sizeof(ap->buf)-8)
		aswrite(ap);
}

/*
 *  output a block
 *
 *  the first 2 bytes of every message are the channel number,
 *  low order byte first.  the third is a possible trailing control
 *  character.
 */
void
asyncoput(Queue *q, Block *bp)
{
	Async *ap = (Async *)q->ptr;
	int c, chan, ctl;

	if(bp->type != M_DATA){
		freeb(bp);
		return;
	}

	/*
	 *  get a whole message before handing bytes to the device
	 */
	if(!putq(q, bp))
		return;

	/*
	 *  one transmitter at a time
	 */
	qlock(&ap->xmit);

	/*
	 *  parse message
	 */
	bp = getq(q);
	if(bp->wptr - bp->rptr < 3){
		freemsg(q, bp);
		qunlock(&ap->xmit);
		return;
	}
	chan = bp->rptr[0] | (bp->rptr[1]<<8);
	ctl = bp->rptr[2];
	bp->rptr += 3;

	/*
	 *  new frame if the channel number has changed
	 */
	if (chan != ap->chan && ap->count > 0)
		asputf(ap, 0);
	ap->chan = chan;

	if (asyncdebug > 1)
		kprint("a%d->(%d)%3.3uo %d\n",
			ap-async, chan, ctl, bp->wptr - bp->rptr);

	/*
	 *  send the 8 bit data
	 */
	for(;;){
		/*
		 *  put in next packet
		 */
		while (bp->rptr < bp->wptr) {
			asputc(ap, c = *bp->rptr++);
			if (c == STUF)
				asputc(ap, 0);
		}

		/*
		 *  get next block 
		 */
		if(bp->flags & S_DELIM){
			freeb(bp);
			break;
		}
		freeb(bp);
		bp = getq(q);
		if(bp==0)
			break;
	}

	/*
	 *  send the control byte if there is one
	 */
	if(ctl){
		asputc(ap, STUF);
		asputc(ap, ctl);
		switch (ctl) {
		case BOT:
		case BOTM:
		case BOTS:
			break;
		default:
			asputf(ap, 1);
		}
	}

	qunlock(&ap->xmit);
	return;
}

/*
 *  Read bytes from the raw input.
 */

void
asdeliver(Async *ap, uchar *p, int n)
{
	Block *bp = 0;
	int chan, c;

	if (!ap->rq)
		return;

	chan = *p++ & 0x7e;
	chan = (chan<<5)|((*p++ & 0x7e)>>1);
	if (chan==0) {
		DPRINT("a%d deliver chan 0\n", ap-async);
		ap->chan0++;
		return;
	}
	for (n-=4; n>0; n--) {
		if (!bp) {
			bp = allocb(n+2);
			bp->flags |= S_DELIM;
			bp->wptr[0] = chan;
			bp->wptr[1] = chan>>8;
			bp->wptr[2] = 0;
			bp->wptr += 3;
		}
		if ((c = *p++) == STUF) {
			--n;
			if ((c = *p++) != 0) {
				bp->rptr[2] = c;
				if (asyncdebug > 1)
					kprint("a%d<-(%d)%3.3uo %d\n",
						ap-async, chan, bp->rptr[2],
						bp->wptr - bp->rptr - 3);
				PUTNEXT(ap->rq, bp);
				bp = 0;
				continue;
			} else
				c = STUF;
		}
		*bp->wptr++ = c;
	}
	if (bp) {
		if (asyncdebug > 1)
			kprint("a%d<-(%d)%3.3uo %d\n",
				ap-async, chan, bp->rptr[2],
				bp->wptr - bp->rptr - 3);
		PUTNEXT(ap->rq, bp);
	}
}

enum { Hunt=0, Framing, Framed, Data, Escape };

static void
asynckproc(void *arg)
{
	Async *ap = (Async *)arg;
	int c, state, count; unsigned short crc;
	uchar buf[MAXFRAME], ibuf[MAXFRAME], *p;
	int nibuf;

	DPRINT("asynckproc %d\n", ap-async);
	nibuf = 0;
	state = Hunt;
Loop:
	if (--nibuf < 0) {
		if (ap->rq)
			nibuf = (*devtab[ap->c->type].read)(ap->c,
					ibuf, sizeof ibuf);
		if (nibuf <= 0) {
			unlock(&ap->kstart);
			wakeup(&ap->r);
			return;
		}
		ap->in += nibuf--;
		p = ibuf;
	}
	c = *p++;
	switch (state) {
	case Hunt:	/* wait for framing byte */
		if (c == FRAME)
			state = Framing;
		break;

	case Framing:	/* saw 1 framing byte after Hunt */
		if (c == FRAME)
			state = Framed;
		else
			state = Hunt;
		break;

	case Framed:	/* saw 2 or more framing bytes */
		if (c == FRAME)
			break;
		state = Data;
		crc = CRCSTART;
		count = 0;
		goto Datachar;

	case Data:	/* mid-frame */
		if (c == FRAME) {
			state = Escape;
			break;
		}
	Datachar:
		if (count >= MAXFRAME) {
			DPRINT("a%d pkt too long\n", ap-async);
			ap->toolong++;
			state = Hunt;
			break;
		}
		crc = CRCFUNC(crc, c);
		buf[count++] = c;
		break;

	case Escape:	/* saw framing byte in Data */
		switch (c) {
		case FRAME:
			if (asyncdebug > 2)
				showframe("in", ap, buf, count);
			if (count < 5) {
				DPRINT("a%d pkt too short\n", ap-async);
				ap->tooshort++;
			} else if (crc != 0) {
				DPRINT("a%d bad crc\n", ap-async);
				ap->badcrc++;
			} else {
				asdeliver(ap, buf, count);
			}
			state = Framed;
			break;
		case 0:
			c = FRAME;
			state = Data;
			goto Datachar;
		default:
			DPRINT("a%d bad escape\n", ap-async);
			ap->badescape++;
			state = Hunt;
			break;
		}
		break;
	}
	goto Loop;
}
