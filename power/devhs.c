#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

typedef struct Hsvme	Hsvme;
typedef struct Device	Device;

enum {
	Maxburst=	1023,		/* maximum transmit burst size */
	Vmevec=		0xd0,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Nhsvme=		1,
};

/*
 *  hsvme datakit board
 */
struct Device {
	ushort	version;
	ushort	pad0x02;
	ushort	vector;
	ushort	pad0x06;
	ushort	csr;
	ushort	pad0x0A;
	ushort	data;
};
#define HSVME		VMEA24SUP(Device, 0xF90000)

struct Hsvme {
	QLock;

	QLock	xmit;
	Device	*addr;
	int	vec;		/* interupt vector */
	Rendez	r;		/* output process */
	Rendez	kr;		/* input kernel process */
	int	chan;		/* current input channel */
	Queue	*rq;		/* read queue */
	uchar	buf[1024];	/* bytes being collected */
	uchar	*wptr;		/* pointer into buf */
	int	kstarted;	/* true if kernel process started */

	/* statistics */

	ulong	parity;		/* parity errors */
	ulong	rintr;		/* rcv interrupts */
	ulong	tintr;		/* transmit interrupts */
	ulong	in;		/* bytes in */
	ulong	out;		/* bytes out */
};

Hsvme hsvme[Nhsvme];

#define ALIVE		0x0001
#define IENABLE		0x0004
#define EXOFLOW		0x0008
#define IRQ		0x0010
#define EMUTE		0x0020
#define EPARITY		0x0040
#define EFRAME		0x0080
#define EROFLOW		0x0100
#define REF		0x0800
#define XFF		0x4000
#define XHF		0x8000

#define FORCEW		0x0008
#define IPL(x)		((x)<<5)
#define NORESET		0xFF00
#define RESET		0x0000

#define CTL		0x0100
#define CHNO		0x0200
#define TXEOD		0x0400
#define NND		0x8000

static void hsvmeintr(int);
static void hsvmekproc(void*);

/*
 *  hsvme stream module definition
 */
static void hsvmeoput(Queue*, Block*);
static void hsvmestopen(Queue*, Stream*);
static void hsvmestclose(Queue*);
Qinfo hsvmeinfo =
{
	nullput,
	hsvmeoput,
	hsvmestopen,
	hsvmestclose,
	"hsvme"
};

/*
 *  restart a VME board
 */
void
hsvmerestart(Hsvme *hp)
{
	Device *addr;

	addr = hp->addr;

	addr->csr = RESET;
	wbflush();
	delay(20);

	/*
	 *  set interrupt vector
	 *  turn on addrice
	 *  set forcew to a known value
	 *  interrupt on level `Intlevel'
	 */
	addr->vector = hp->vec;
	addr->csr = NORESET|IPL(Intlevel)|IENABLE|ALIVE;
	wbflush();
	delay(1);
	addr->csr = NORESET|IPL(Intlevel)|FORCEW|IENABLE|ALIVE;
	wbflush();
	delay(1);
}

/*
 *  reset all vme boards
 */
void
hsvmereset(void)
{
	int i;
	Hsvme *hp;

	for(i=0; i<Nhsvme; i++){
		hsvme[i].addr = HSVME+i;
		hsvme[i].vec = Vmevec+i;
		hsvme[i].addr->csr = RESET;
		setvmevec(hsvme[i].vec, hsvmeintr);
	}	
	wbflush();
	delay(20);
}

void
hsvmeinit(void)
{
}

/*
 *  enable the device for interrupts, spec is the device number
 */
Chan*
hsvmeattach(char *spec)
{
	Hsvme *hp;
	int i;
	Chan *c;

	i = strtoul(spec, 0, 0);
	if(i >= Nhsvme)
		error(Ebadarg);
	hp = &hsvme[i];
	hsvmerestart(hp);

	c = devattach('h', spec);
	c->dev = i;
	c->qid.path = CHDIR;
	return c;
}

Chan*
hsvmeclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
hsvmewalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, streamgen);
}

void	 
hsvmestat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, streamgen);
}

Chan*
hsvmeopen(Chan *c, int omode)
{
	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}else
		streamopen(c, &hsvmeinfo);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
hsvmecreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
hsvmeclose(Chan *c)
{
	if(c->qid.path != CHDIR)
		streamclose(c);
}

long	 
hsvmeread(Chan *c, void *buf, long n, ulong offset)
{
	return streamread(c, buf, n);
}

long	 
hsvmewrite(Chan *c, void *buf, long n, ulong offset)
{
	return streamwrite(c, buf, n, 0);
}

void	 
hsvmeremove(Chan *c)
{
	error(Eperm);
}

void	 
hsvmewstat(Chan *c, char *dp)
{
	error(Eperm);
}

/*
 *	the stream routines
 */

/*
 *  create the kernel process for input
 */
static void
hsvmestopen(Queue *q, Stream *s)
{
	Hsvme *hp;
	char name[32];

	hp = &hsvme[s->dev];
	sprint(name, "hsvme%d", s->dev);
	q->ptr = q->other->ptr = hp;
	hp->rq = q;
	kproc(name, hsvmekproc, hp);
}

/*
 *  kill off the kernel process
 */
static int
kdead(void *arg)
{
	Hsvme *hp;

	hp = (Hsvme *)arg;
	return hp->kstarted == 0;
}
static void
hsvmestclose(Queue * q)
{
	Hsvme *hp;

	hp = (Hsvme *)q->ptr;
	qlock(hp);
	hp->rq = 0;
	qunlock(hp);
	wakeup(&hp->kr);
	sleep(&hp->r, kdead, hp);
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

/*
 *  return true if the output fifo is at least half empty.
 *  the implication is that it can take at least another 1000 byte burst.
 */
static int
halfempty(void *arg)
{
	Device *addr;

	addr = (Device*)arg;
	return addr->csr & XHF;
}

/*
 *  output a block
 *
 *  the first 2 bytes of every message are the channel number,
 *  low order byte first.  the third is a possible trailing control
 *  character.
 */
void
hsvmeoput(Queue *q, Block *bp)
{
	Device *addr;
	Hsvme *hp;
	int burst;
	int chan;
	int ctl;
	int n;

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
	hp = (Hsvme *)q->ptr;
	qlock(&hp->xmit);
	addr = hp->addr;

	/*
	 *  parse message
	 */
	bp = getq(q);
	if(bp->wptr - bp->rptr < 3){
		freemsg(q, bp);
		qunlock(&hp->xmit);
		return;
	}
	chan = CHNO | bp->rptr[0] | (bp->rptr[1]<<8);
	ctl = bp->rptr[2];
	bp->rptr += 3;

	/*
	 *  send the 8 bit data, burst are up to Maxburst (9-bit) bytes long
	 */
	if(!(addr->csr & XHF))
		sleep(&hp->r, halfempty, addr);
/*	print("->%.2uo\n", CHNO|chan);/**/
	addr->data = CHNO|chan;
	burst = Maxburst;
	while(bp){
		if(burst == 0){
			addr->data = TXEOD;
/*			print("->%.2uo\n", TXEOD); /**/
			if(!(addr->csr & XHF))
				sleep(&hp->r, halfempty, addr);
/*			print("->%.2uo\n", CHNO|chan); /**/
			addr->data = CHNO|chan;
			burst = Maxburst;
		}
		n = bp->wptr - bp->rptr;
		if(n > burst)
			n = burst;
		burst -= n;
		while(n--){
/*			print("->%.2uo\n", *bp->rptr); /**/
			addr->data = *bp->rptr++;
		}
		if(bp->rptr >= bp->wptr){
			if(bp->flags & S_DELIM){
				freeb(bp);
				break;
			}
			freeb(bp);
			bp = getq(q);
		}
	}

	/*
	 *  send the control byte if there is one
	 */
	if(ctl){
/*		print("->%.2uo\n", CTL|ctl); /**/
		addr->data = CTL|ctl;
	}

	/*
	 *  start the fifo emptying
	 */
/*	print("->%.2uo\n", TXEOD); /**/
	addr->data = TXEOD;

	qunlock(&hp->xmit);
}

/*
 *  return true if the input fifo is non-empty
 */
static int
notempty(void *arg)
{
	Device *addr;

	addr = (Device *)arg;
	return addr->csr & REF;
}

/*
 *  fill a block with what is currently buffered and send it upstream
 */
static void
upstream(Hsvme *hp, unsigned int ctl)
{
	int n;
	Block *bp;

	n = hp->wptr - hp->buf;
	bp = allocb(3 + n);
	bp->wptr[0] = hp->chan;
	bp->wptr[1] = hp->chan>>8;
	bp->wptr[2] = ctl;
	if(n)
		memmove(&bp->wptr[3], hp->buf, n);
	bp->wptr += 3 + n;
	bp->flags |= S_DELIM;
	PUTNEXT(hp->rq, bp);
	hp->wptr = hp->buf;
}

/*
 *  Read bytes from the input fifo.  Since we take an interrupt every
 *  time the fifo goes non-empty, we need to waste time to let the
 *  fifo fill up.
 */
static void
hsvmekproc(void *arg)
{
	Hsvme *hp;
	Device *addr;
	unsigned int c;
	int locked;

	hp = (Hsvme *)arg;
	addr = hp->addr;
	hp->kstarted = 1;
	hp->wptr = hp->buf;

	locked = 0;
	if(waserror()){
		if(locked)
			qunlock(hp);
		hp->kstarted = 0;
		wakeup(&hp->r);
		return;
	}

	for(;;){
		/*
		 *  die if the device is closed
		 */
		USED(locked);		/* so locked = 0 and locked = 1 stay */
		qlock(hp);
		locked = 1;
		if(hp->rq == 0){
			qunlock(hp);
			hp->kstarted = 0;
			wakeup(&hp->r);
			poperror();
			return;
		}

		/*
		 *  0xFFFF means an empty fifo
		 */
		while ((c = addr->data) != 0xFFFF) {
			hp->in++;
			if(c & CHNO){
				c &= 0x1FF;
				if(hp->chan == c)
					continue;
				/*
				 *  new channel, put anything saved upstream
				 */
				if(hp->wptr - hp->buf != 0)
					upstream(hp, 0);
				hp->chan = c;
			} else if(c & NND){
				/*
				 *  ctl byte, this ends a message
				 */
				upstream(hp, c);
			} else {
				/*
				 *  data byte, put in local buffer
				 */
				*hp->wptr++ = c;
				if(hp->wptr == &hp->buf[sizeof hp->buf])
					upstream(hp, 0);
			}
		}
		USED(locked);
		qunlock(hp);
		locked = 0;

		/*
		 *  sleep if input fifo empty
		 */
		if(!notempty(addr))
			sleep(&hp->kr, notempty, addr);
	}
}

/*
 *  only one flavor interrupt.   we have to use the less than half full
 *  and not empty bits to figure out whom to wake.
 */
static void
hsvmeintr(int vec)
{
	ushort csr;
	Device *addr;
	Hsvme *hp;

	hp = &hsvme[vec - Vmevec];
	if(hp < hsvme || hp > &hsvme[Nhsvme]){
		print("bad hsvme vec\n");
		return;
	}
	csr = hp->addr->csr;

	if (csr & REF) {
		hp->rintr++;
		wakeup(&hp->kr);
	}
	if (csr & XHF) {
		hp->tintr++;
		wakeup(&hp->r);
	}
	if ((csr^XFF) & (XFF|EROFLOW|EFRAME|EPARITY|EXOFLOW)) {
		hp->parity++;
		hsvmerestart(hp);
		print("hsvme %d: reset, csr = 0x%ux\n",
			vec - Vmevec, csr);
	}
}
