/*
 * UHCI USB driver
 *	(c) 1998, 1999 C H Forsyth, forsyth@caldo.demon.co.uk
 * to do:
 *	endpoint open/close
 *	build Endpt on open from attributes stored in Udev?
 *	build data0/data1 rings for bulk and interrupt endpoints
 *	endpoint TD rings (can there be prefetch?)
 *	hubs?
 *	special handling of isochronous traffic?
 *	is use of Queues justified? (could have client clean TD rings on wakeup)
 *	bandwidth check
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#define Chatty	1
#define DPRINT if(Chatty)print
#define XPRINT if(debug)print

static int debug = 0;

/*
 * USB packet definitions
 */
enum {
	TokIN = 0x69,
	TokOUT = 0xE1,
	TokSETUP = 0x2D,

	/* request type */
	RH2D = 0<<7,
	RD2H = 1<<7,
	Rstandard = 0<<5,
	Rclass = 1<<5,
	Rvendor = 2<<5,
	Rdevice = 0,
	Rinterface = 1,
	Rendpt = 2,
	Rother = 3,
};

typedef uchar byte;

typedef struct Ctlr Ctlr;
typedef struct Endpt Endpt;
typedef struct QTree QTree;
typedef struct Udev Udev;

/*
 * UHCI hardware structures, aligned on 16-byte boundary
 */
typedef struct QH QH;
typedef struct TD TD;

#define Class(csp)		((csp)&0xff)
#define Subclass(csp)	(((csp)>>8)&0xff)
#define Proto(csp)		(((csp)>>16)&0xff)
#define CSP(c, s, p)	((c) | ((s)<<8) | ((p)<<16))

struct TD {
	ulong	link;
	ulong	status;	/* controller r/w */
	ulong	dev;
	ulong	buffer;

	/* software */
	ulong	flags;
	Block*	bp;
	Endpt*	ep;
	TD*	next;
};
#define	TFOL(p)	((TD*)KADDR((ulong)(p) & ~0xF))

struct QH {
	ulong	head;
	ulong	entries;	/* address of next TD or QH to process (updated by controller) */

	/* software */
	QH*		hlink;
	TD*		first;
	QH*		next;		/* free list */
	TD*		last;
	ulong	_d1;		/* fillers */
	ulong	_d2;
};
#define	QFOL(p)	((QH*)KADDR((ulong)(p) & ~0xF))

/*
 * UHCI interface registers and bits
 */
enum {
	/* i/o space */
	Cmd = 0,
	Status = 2,
	Usbintr = 4,
	Frnum = 6,
	Flbaseadd = 8,
	SOFMod = 0xC,
	Portsc0 = 0x10,
	Portsc1 = 0x12,

	/* port status */
	Suspend = 1<<12,
	PortReset = 1<<9,
	SlowDevice = 1<<8,
	ResumeDetect = 1<<6,
	PortChange = 1<<3,	/* write 1 to clear */
	PortEnable = 1<<2,
	StatusChange = 1<<1,	/* write 1 to clear */
	DevicePresent = 1<<0,

	FRAMESIZE=	4096,	/* fixed by hardware; aligned to same */
	NFRAME = 	FRAMESIZE/4,

	Vf = 1<<2,	/* TD only */
	IsQH = 1<<1,
	Terminate = 1<<0,

	/* TD.status */
	SPD = 1<<29,
	ErrLimit0 = 0<<27,
	ErrLimit1 = 1<<27,
	ErrLimit2 = 2<<27,
	ErrLimit3 = 3<<27,
	LowSpeed = 1<<26,
	IsoSelect = 1<<25,
	IOC = 1<<24,
	Active = 1<<23,
	Stalled = 1<<22,
	DataBufferErr = 1<<21,
	Babbling = 1<<20,
	NAKed = 1<<19,
	CRCorTimeout = 1<<18,
	BitstuffErr = 1<<17,
	AnyError = (Stalled | DataBufferErr | Babbling | NAKed | CRCorTimeout | BitstuffErr),

	/* TD.dev */
	IsDATA1 =	1<<19,

	/* TD.flags (software) */
	CancelTD=	1<<0,
};

/*
 * software structures
 */
struct QTree {
	QLock;
	int	nel;
	int	depth;
	QH*	root;
	ulong*	bw;
};

#define	GET2(p)	((((p)[1]&0xFF)<<8)|((p)[0]&0xFF))
#define	PUT2(p,v)	(((p)[0] = (v)), ((p)[1] = (v)>>8))

/*
 * active USB device
 */
struct Udev {
	Ref;
	Lock;
	int		x;	/* index in usbdev[] */
	int		busy;
	int		state;
	int		id;
	byte	port;		/* port number on connecting hub */
	ulong	csp;
	int		ls;
	int		npt;
	Endpt*	ep[16];		/* active end points */
	Udev*	ports;		/* active ports, if hub */
	Udev*	next;		/* next device on this hub */
};

/* device parameters */
enum {
	/* Udev.state */
	Disabled = 0,
	Attached,
	Enabled,
	Assigned,
	Configured,

	/* Udev.class */
	Noclass = 0,
	Hubclass = 9,
};

static char *devstates[] = {
	[Disabled]		"Disabled",
	[Attached]		"Attached",
	[Enabled]		"Enabled",
	[Assigned]		"Assigned",
	[Configured]	"Configured",
};

/*
 * device endpoint
 */
struct Endpt {
	Ref;
	Lock;
	int		x;	/* index in Udev.ep */
	int		id;		/* hardware endpoint address */
	int		maxpkt;	/* maximum packet size (from endpoint descriptor) */
	int		data01;	/* 0=DATA0, 1=DATA1 */
	byte	eof;
	ulong	csp;
	byte	mode;	/* OREAD, OWRITE, ORDWR */
	byte	nbuf;	/* number of buffers allowed */
	byte	periodic;
	byte	iso;
	byte	debug;
	byte	active;	/* listed for examination by interrupts */
	int		sched;	/* schedule index; -1 if undefined or aperiodic */
	int		setin;
	ulong	bw;	/* bandwidth requirement */
	int		pollms;	/* polling interval in msec */
	QH*		epq;	/* queue of TDs for this endpoint */

	QLock	rlock;
	Rendez	rr;
	Queue*	rq;
	QLock	wlock;
	Rendez	wr;
	Queue*	wq;

	int		ntd;
	char*	err;

	Udev*	dev;	/* owning device */

	Endpt*	activef;	/* active endpoint list */

	ulong	nbytes;
	ulong	nblocks;
};

struct Ctlr {
	Lock;	/* protects state shared with interrupt (eg, free list) */
	int	io;
	ulong*	frames;	/* frame list */
	int	idgen;	/* version number to distinguish new connections */
	QLock	resetl;	/* lock controller during USB reset */

	TD*	tdpool;
	TD*	freetd;
	QH*	qhpool;
	QH*	freeqh;

	QTree*	tree;	/* tree for periodic Endpt i/o */
	QH*	ctlq;	/* queue for control i/o */
	QH*	bwsop;	/* empty bandwidth sop (to PIIX4 errata specifications) */
	QH*	bulkq;	/* queue for bulk i/o (points back to bandwidth sop) */
	QH*	recvq;	/* receive queues for bulk i/o */

	Udev*	ports[2];
};
#define	IN(x)	ins(ub->io+(x))
#define	OUT(x, v)	outs(ub->io+(x), (v))

static	Ctlr	ubus;
static	char	Estalled[] = "usb endpoint stalled";

static	QLock	usbstate;	/* protects name space state */
static	Udev*	usbdev[32];
static struct {
	Lock;
	Endpt*	f;
} activends;

static long readusb(Endpt*, void*, long);
static long writeusb(Endpt*, void*, long, int);

static TD *
alloctd(Ctlr *ub)
{
	TD *t;

	ilock(ub);
	t = ub->freetd;
	if(t == nil)
		panic("alloctd");	/* TO DO */
	ub->freetd = t->next;
	t->next = nil;
	iunlock(ub);
	t->ep = nil;
	t->bp = nil;
	t->status = 0;
	t->link = Terminate;
	t->buffer = 0;
	t->flags = 0;
	return t;
}

static void
freetd(TD *t)
{
	Ctlr *ub;

	ub = &ubus;
	t->ep = nil;
	if(t->bp)
		freeb(t->bp);
	t->bp = nil;
	ilock(ub);
	t->buffer = 0xdeadbeef;
	t->next = ub->freetd;
	ub->freetd = t;
	iunlock(ub);
}

static void
dumpdata(Block *b, int n)
{
	int i;

	XPRINT("\tb %8.8lux[%d]: ", (ulong)b->rp, n);
	if(n > 16)
		n = 16;
	for(i=0; i<n; i++)
		XPRINT(" %2.2ux", b->rp[i]);
	XPRINT("\n");
}

static void
dumptd(TD *t, int follow)
{
	int i, n;
	char buf[20], *s;
	TD *t0;

	t0 = t;
	while(t){
		i = t->dev & 0xFF;
		if(i == TokOUT || i == TokSETUP)
			n = ((t->dev>>21) + 1) & 0x7FF;
		else if((t->status & Active) == 0)
			n = (t->status + 1) & 0x7FF;
		else
			n = 0;
		s = buf;
		if(t->status & Active)
			*s++ = 'A';
		if(t->status & Stalled)
			*s++ = 'S';
		if(t->status & DataBufferErr)
			*s++ = 'D';
		if(t->status & Babbling)
			*s++ = 'B';
		if(t->status & NAKed)
			*s++ = 'N';
		if(t->status & CRCorTimeout)
			*s++ = 'T';
		if(t->status & BitstuffErr)
			*s++ = 'b';
		if(t->status & LowSpeed)
			*s++ = 'L';
		*s = 0;
		XPRINT("td %8.8lux: ", t);
		XPRINT("l=%8.8lux s=%8.8lux d=%8.8lux b=%8.8lux %8.8lux f=%8.8lux\n",
			t->link, t->status, t->dev, t->buffer, t->bp?(ulong)t->bp->rp:0, t->flags);
		XPRINT("\ts=%s,ep=%ld,d=%ld,D=%ld\n", buf, (t->dev>>15)&0xF, (t->dev>>8)&0xFF, (t->dev>>19)&1);
		if(debug && t->bp && (t->flags & CancelTD) == 0)
			dumpdata(t->bp, n);
		if(!follow || t->link & Terminate || t->link & IsQH)
			break;
		t = TFOL(t->link);
		if(t == t0)
			break;	/* looped */
	}
}

static TD *
alloctde(Endpt *e, int pid, int n)
{
	TD *t;
	int tog, id;

	t = alloctd(&ubus);
	id = (e->x<<7)|(e->dev->x&0x7F);
	tog = 0;
	if(e->data01 && pid != TokSETUP)
		tog = IsDATA1;
	t->ep = e;
	t->status = ErrLimit3 | Active | IOC;	/* or put IOC only on last? */
	if(e->dev->ls)
		t->status |= LowSpeed;
	t->dev = ((n-1)<<21) | ((id&0x7FF)<<8) | pid | tog;
	return t;
}

static QH *
allocqh(Ctlr *ub)
{
	QH *qh;

	ilock(ub);
	qh = ub->freeqh;
	if(qh == nil)
		panic("allocqh");	/* TO DO */
	ub->freeqh = qh->next;
	qh->next = nil;
	iunlock(ub);
	qh->head = Terminate;
	qh->entries = Terminate;
	qh->hlink = nil;
	qh->first = nil;
	qh->last = nil;
	return qh;
}

static void
freeqh(Ctlr *ub, QH *qh)
{
	ilock(ub);
	qh->next = ub->freeqh;
	ub->freeqh = qh;
	iunlock(ub);
}

static void
dumpqh(QH *q)
{
	int i;
	QH *q0;

	q0 = q;
	for(i = 0; q != nil && i < 10; i++){
		XPRINT("qh %8.8lux: %8.8lux %8.8lux\n", q, q->head, q->entries);
		if((q->entries & Terminate) == 0)
			dumptd(TFOL(q->entries), 1);
		if(q->head & Terminate)
			break;
		if((q->head & IsQH) == 0){
			XPRINT("head:");
			dumptd(TFOL(q->head), 1);
			break;
		}
		q = QFOL(q->head);
		if(q == q0)
			break;	/* looped */
	}
}

static void
queuetd(Ctlr *ub, QH *q, TD *t, int vf)
{
	TD *lt;

	for(lt = t; lt->next != nil; lt = lt->next)
		lt->link = PADDR(lt->next) | vf;
	lt->link = Terminate;
	ilock(ub);
	if(q->first != nil){
		q->last->link = PADDR(t) | vf;
		q->last->next = t;
	}else{
		q->first = t;
		q->entries = PADDR(t);
	}
	q->last = lt;
	dumpqh(q);
	iunlock(ub);
}

static void
cleantd(TD *t, int discard)
{
	Block *b;
	int n, err;

	XPRINT("cleanTD: %8.8lux %8.8lux %8.8lux %8.8lux\n", t->link, t->status, t->dev, t->buffer);
	if(t->ep != nil && t->ep->debug)
		dumptd(t, 0);
	if(t->status & Active)
		panic("cleantd Active");
	err = t->status & (AnyError&~NAKed);
	/* TO DO: on t->status&AnyError, q->entries will not have advanced */
	if (err)
		XPRINT("cleanTD: Error %8.8lux %8.8lux %8.8lux %8.8lux\n", t->link, t->status, t->dev, t->buffer);
	switch(t->dev&0xFF){
	case TokIN:
		if(discard || (t->flags & CancelTD) || t->ep == nil || t->ep->x!=0&&err){
			if(t->ep != nil){
				if(err != 0)
					t->ep->err = err==Stalled? Estalled: Eio;
				wakeup(&t->ep->rr);	/* in case anyone cares */
			}
			break;
		}
		b = t->bp;
		n = (t->status + 1) & 0x7FF;
		if(n > b->lim - b->wp)
			n = 0;
		b->wp += n;
		if(Chatty)
			dumpdata(b, n);
		t->bp = nil;
		t->ep->nbytes += n;
		t->ep->nblocks++;
		qpass(t->ep->rq, b);	/* TO DO: flow control */
		wakeup(&t->ep->rr);	/* TO DO */
		break;
	case TokSETUP:
		XPRINT("cleanTD: TokSETUP %lux\n", &t->ep);
		/* don't really need to wakeup: subsequent IN or OUT gives status */
		if(t->ep != nil) {
			wakeup(&t->ep->wr);	/* TO DO */
			XPRINT("cleanTD: wakeup %lux\n", &t->ep->wr);
		}
		break;
	case TokOUT:
		/* TO DO: mark it done somewhere */
		XPRINT("cleanTD: TokOut %lux\n", &t->ep);
		if(t->ep != nil){
			if(t->bp){
				n = BLEN(t->bp);
				t->ep->nbytes += n;
				t->ep->nblocks++;
			}
			if(t->ep->x!=0 && err != 0)
				t->ep->err = err==Stalled? Estalled: Eio;
			if(--t->ep->ntd < 0)
				panic("cleantd ntd");
			wakeup(&t->ep->wr);	/* TO DO */
			XPRINT("cleanTD: wakeup %lux\n", &t->ep->wr);
		}
		break;
	}
	freetd(t);
}

static void
cleanq(QH *q, int discard, int vf)
{
	TD *t, *tp;
	Ctlr *ub;

	ub = &ubus;
	ilock(ub);
	tp = nil;
	for(t = q->first; t != nil;){
		XPRINT("cleanq: %8.8lux %8.8lux %8.8lux %8.8lux %8.8lux %8.8lux\n", t->link, t->status, t->dev, t->buffer, t->flags, t->next);
		if(t->status & Active){
			if(t->status & NAKed){
				t->status = (t->status & ~NAKed) | IOC;	/* ensure interrupt next frame */
				tp = t;
				t = t->next;
				continue;
			}
			if(t->flags & CancelTD){
				XPRINT("cancelTD: %8.8lux\n", (ulong)t);
				t->status = (t->status & ~Active) | IOC;	/* ensure interrupt next frame */
				tp = t;
				t = t->next;
				continue;
			}
tp = t;
t = t->next;
continue;
			break;
		}
		t->status &= ~IOC;
		if (tp == nil) {
			q->first = t->next;
			if(q->first != nil)
				q->entries = PADDR(q->first);
			else
				q->entries = Terminate;
		} else {
			tp->next = t->next;
			if (t->next != nil)
				tp->link = PADDR(t->next) | vf;
			else
				tp->link = Terminate;
		}
		if (q->last == t)
			q->last = tp;
		iunlock(ub);
		cleantd(t, discard);
		ilock(ub);
		if (tp)
			t = tp->next;
		else
			t = q->first;
		XPRINT("t = %8.8lux\n", t);
		dumpqh(q);
	}
	iunlock(ub);
}

static void
canceltds(Ctlr *ub, QH *q, Endpt *e)
{
	TD *t;

	if(q != nil){
		ilock(ub);
		for(t = q->first; t != nil; t = t->next)
			if(t->ep == e)
				t->flags |= CancelTD;
		iunlock(ub);
		XPRINT("cancel:\n");
		dumpqh(q);
	}
}

static void
eptcancel(Endpt *e)
{
	Ctlr *ub;

	if(e == nil)
		return;
	ub = &ubus;
	canceltds(ub, e->epq, e);
	canceltds(ub, ub->ctlq, e);
	canceltds(ub, ub->bulkq, e);
}

static void
eptactivate(Endpt *e)
{
	ilock(&activends);
	if(e->active == 0){
		e->active = 1;
		e->activef = activends.f;
		activends.f = e;
	}
	iunlock(&activends);
}

static void
eptdeactivate(Endpt *e)
{
	Endpt **l;

	/* could be O(1) but not worth it yet */
	ilock(&activends);
	if(e->active){
		e->active = 0;
		for(l = &activends.f; *l != e; l = &(*l)->activef)
			if(*l == nil){
				iunlock(&activends);
				panic("usb eptdeactivate");
			}
		*l = e->activef;
	}
	iunlock(&activends);
}

static void
queueqh(QH *qh) {
	QH *q;
	Ctlr *ub;

	ub = &ubus;
	// See if it's already queued
	for (q = ub->recvq->next; q; q = q->hlink)
		if (q == qh)
			return;
	if ((qh->hlink = ub->recvq->next) == nil)
		qh->head = Terminate;
	else
		qh->head = PADDR(ub->recvq->next) | IsQH;
	ub->recvq->next = qh;
	ub->recvq->entries = PADDR(qh) | IsQH;
}

static QH*
qxmit(Endpt *e, Block *b, int pid)
{
	TD *t;
	int n, vf;
	Ctlr *ub;
	QH *qh;

	if(b != nil){
		n = BLEN(b);
		t = alloctde(e, pid, n);
		t->bp = b;
		t->buffer = PADDR(b->rp);
	}else
		t = alloctde(e, pid, 0);
	ub = &ubus;
	ilock(ub);
	e->ntd++;
	iunlock(ub);
	if(e->debug) pprint("QTD: %8.8lux n=%ld\n", t, b?BLEN(b): 0);
	vf = 0;
	if(e->x == 0){
		qh = ub->ctlq;
		vf = 0;
	}else if((qh = e->epq) == nil || e->mode != OWRITE){
		qh = ub->bulkq;
		vf = Vf;
	}
	queuetd(ub, qh, t, vf);
	return qh;
}

static QH*
qrcv(Endpt *e)
{
	TD *t;
	Block *b;
	Ctlr *ub;
	QH *qh;
	int vf;

	t = alloctde(e, TokIN, e->maxpkt);
	b = allocb(e->maxpkt);
	t->bp = b;
	t->buffer = PADDR(b->wp);
	ub = &ubus;
	vf = 0;
	if(e->x == 0){
		qh = ub->ctlq;
	}else if((qh = e->epq) == nil || e->mode != OREAD){
		qh = ub->bulkq;
		vf = Vf;
	}
	queuetd(ub, qh, t, vf);
	return qh;
}

static Block *
usbreq(int type, int req, int value, int offset, int count)
{
	Block *b;

	b = allocb(8);
	b->wp[0] = type;
	b->wp[1] = req;
	PUT2(b->wp+2, value);
	PUT2(b->wp+4, offset);
	PUT2(b->wp+6, count);
	b->wp += 8;
	return b;
}

/*
 * return smallest power of 2 >= n
 */
static int
flog2(int n)
{
	int i;

	for(i=0; (1<<i)<n; i++)
		;
	return i;
}

/*
 * build the periodic scheduling tree:
 * framesize must be a multiple of the tree size
 */
static QTree *
mkqhtree(ulong *frame, int framesize, int maxms)
{
	int i, n, d, o, leaf0, depth;
	QH *tree, *qh;
	QTree *qt;

	depth = flog2(maxms);
	n = (1<<(depth+1))-1;
	qt = mallocz(sizeof(*qt), 1);
	if(qt == nil)
		return nil;
	qt->nel = n;
	qt->depth = depth;
	qt->bw = mallocz(n*sizeof(qt->bw), 1);
	if(qt->bw == nil){
		free(qt);
		return nil;
	}
	tree = xspanalloc(n*sizeof(QH), 16, 0);
	if(tree == nil){
		free(qt);
		return nil;
	}
	qt->root = tree;
	tree->head = Terminate;	/* root */
	tree->entries = Terminate;
	for(i=1; i<n; i++){
		qh = &tree[i];
		qh->head = PADDR(&tree[(i-1)/2]) | IsQH;
		qh->entries = Terminate;
	}
	/* distribute leaves evenly round the frame list */
	leaf0 = n/2;
	for(i=0; i<framesize; i++){
		o = 0;
		for(d=0; d<depth; d++){
			o <<= 1;
			if(i & (1<<d))
				o |= 1;
		}
		if(leaf0+o >= n){
			XPRINT("leaf0=%d o=%d i=%d n=%d\n", leaf0, o, i, n);
			break;
		}
		frame[i] = PADDR(&tree[leaf0+o]) | IsQH;
	}
	return qt;
}

static void
dumpframe(int f, int t)
{
	QH *q, *tree;
	ulong p, *frame;
	int i, n;

	n = ubus.tree->nel;
	tree = ubus.tree->root;
	frame = ubus.frames;
	if(f < 0)
		f = 0;
	if(t < 0)
		t = 32;
	for(i=f; i<t; i++){
		XPRINT("F%.2d %8.8lux %8.8lux\n", i, frame[i], QFOL(frame[i])->head);
		for(p=frame[i]; (p & IsQH) && (p &Terminate) == 0; p = q->head){
			q = QFOL(p);
			if(!(q >= tree && q < &tree[n])){
				XPRINT("Q: p=%8.8lux out of range\n", p);
				break;
			}
			XPRINT("  -> %8.8lux h=%8.8lux e=%8.8lux\n", p, q->head, q->entries);
		}
	}
}

static int
pickschedq(QTree *qt, int pollms, ulong bw, ulong limit)
{
	int i, j, d, ub, q;
	ulong best, worst, total;

	d = flog2(pollms);
	if(d > qt->depth)
		d = qt->depth;
	q = -1;
	worst = 0;
	best = ~0;
	ub = (1<<(d+1))-1;
	for(i=(1<<d)-1; i<ub; i++){
		total = qt->bw[0];
		for(j=i; j > 0; j=(j-1)/2)
			total += qt->bw[j];
		if(total < best){
			best = total;
			q = i;
		}
		if(total > worst)
			worst = total;
	}
	if(worst+bw >= limit)
		return -1;
	return q;
}

static int
schedendpt(Endpt *e)
{
	Ctlr *ub;
	QH *qh;
	int q;

	if(!e->periodic || e->sched
