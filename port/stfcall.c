#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"fcall.h"

#define DPRINT 	if(fcalldebug)kprint

int fcalldebug = 0;

typedef struct Fcalld	Fcalld;

struct Fcalld{
	int	dev;		/* ref. for debug output */
	int	state;
	int	type;		/* of current message */
	int	need;		/* bytes remaining in current message */
	int	nhdr;		/* bytes of header treasured up in hdr */
	uchar	hdr[16];
};

enum {
	Startup, Startup1, Begin, Header, Data
};

/*
 *  fcall stream module definition
 */
static void fcalliput(Queue*, Block*);
static void fcalloput(Queue*, Block*);
static void fcallopen(Queue*, Stream*);
static void fcallclose(Queue*);
static void fcallreset(void);
Qinfo fcallinfo =
{
	fcalliput,
	fcalloput,
	fcallopen,
	fcallclose,
	"fcall",
	fcallreset
};

static Block *	putmsg(Queue*, Block*, int);

static uchar msglen[] = {
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  3,  3,  3,  3,  0, 67,  5,  3, 89, 13,  7,  5, 33, 13,
	  6, 13, 38, 13, 15,  8, 16,  7,  5,  5,  5,  5,  5,121,121,  5,
	 35, 13,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};
static void
fcallreset(void)
{}

static void
fcallopen(Queue *q, Stream *s)
{
	Fcalld *f;

	DPRINT("fcallopen %d\n", s->dev);

	q->ptr = allocb(sizeof(Fcalld));
	f = (Fcalld *)((Block *)q->ptr)->base;
	f->dev = s->dev;
	f->state = Startup;
	f->type = 0;
	f->need = 0;
	f->nhdr = 0;
}

static void
fcallclose(Queue * q)
{
	Fcalld *f = (Fcalld *)((Block *)q->ptr)->base;

	DPRINT("fcallstclose %d\n", f->dev);
	freeb(q->ptr);
	q->ptr = 0;
}

void
fcalloput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}


static void
fcalliput(Queue *q, Block *bp)
{
	Fcalld *f = (Fcalld *)((Block *)q->ptr)->base;
	int len, n;

	len = BLEN(bp);
	DPRINT("fcalliput %d: blen=%d\n", f->dev, len);
	if(bp->type != M_DATA){
		DPRINT("fcalliput %d: type=%d\n", f->dev, bp->type);
		PUTNEXT(q, bp);
		return;
	}
	bp->flags &= ~S_DELIM;
	if(len == 0){
		freeb(bp);
		return;
	}
	while(len > 0)switch(f->state){
	case Startup:
		if (len == 1 && bp->rptr[0] == 'O'){
			DPRINT("fcalliput %d: O\n", f->dev);
			PUTNEXT(q, bp);
			f->state = Startup1;
			return;
		}
		if(bp->rptr[0] == 'O' && bp->rptr[1] == 'K'){
			DPRINT("fcalliput %d: OK\n", f->dev);
			bp = putmsg(q, bp, 2);
			len -= 2;
		}
		f->state = Begin;
		break;

	case Startup1:
		if(bp->rptr[0] == 'K'){
			DPRINT("fcalliput %d: K\n", f->dev);
			bp = putmsg(q, bp, 1);
			len -= 1;
			f->state = Begin;
			break;
		}
		f->type = 'O';
		f->need = msglen['O']-1;
		f->state = Data;
		DPRINT("fcalliput %d: type=%d, need=%d\n",
			f->dev, f->type, f->need);
		break;

	case Begin:
		f->type = bp->rptr[0];
		f->need = msglen[f->type];
		f->nhdr = 0;
		if(f->type == Twrite || f->type == Rread)
			f->state = Header;
		else
			f->state = Data;
		DPRINT("fcalliput %d: type=%d, need=%d\n",
			f->dev, f->type, f->need);
		break;

	case Header:
		n = f->need;
		if(n > len)
			n = len;
		memmove(&f->hdr[f->nhdr], bp->rptr, n);
		f->nhdr += n;
		DPRINT("fcalliput %d: nhdr=%d\n",
			f->dev, f->nhdr);
		if(n == f->need){
			f->need += f->hdr[f->nhdr-3];
			f->need += f->hdr[f->nhdr-2] << 8;
			f->state = Data;
			DPRINT("fcalliput %d: need=%d\n",
				f->dev, f->need);
		}
		/* fall through */

	case Data:
		if(f->need > len){
			f->need -= len;
			PUTNEXT(q, bp);
			return;
		}
		bp = putmsg(q, bp, f->need);
		len -= f->need;
		f->state = Begin;
		break;
	}
}

static Block *
putmsg(Queue *q, Block *bp, int n)
{
	Block *xbp;
	int k;

	DPRINT("putmsg: n=%d\n\n", n);
	k = BLEN(bp) - n;
	if(k == 0){
		bp->flags |= S_DELIM;
		PUTNEXT(q, bp);
		return 0;
	}
	if(n <= k){
		xbp = allocb(n);
		memmove(xbp->wptr, bp->rptr, n);
		xbp->wptr += n;
		bp->rptr += n;
		xbp->flags |= S_DELIM;
		PUTNEXT(q, xbp);
		return bp;
	}
	xbp = allocb(k);
	memmove(xbp->wptr, bp->rptr+n, k);
	xbp->wptr += k;
	bp->wptr -= k;
	bp->flags |= S_DELIM;
	PUTNEXT(q, bp);
	return xbp;
}
