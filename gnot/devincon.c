#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

typedef struct Incon	Incon;
typedef struct Device	Device;

#define NOW (MACHP(0)->ticks*MS2HZ)

#define DPRINT if(0)

enum {
	Minstation=	2,	/* lowest station # to poll */
	Maxstation=	15,	/* highest station # to poll */
	Nincon=		1,	/* number of incons */
	Nraw=		1024,	/* size of raw input buffer */
};

/*
 *  incon datakit board
 */
struct Device {
	uchar	cdata;
#define	cpolln	cdata
	uchar	u0;
	uchar	cstatus;
	uchar	u1;
	uchar	creset;
	uchar	u2;
	uchar	csend;
	uchar	u3;
	ushort	data_cntl;	/* data is high byte, cntl is low byte */
	uchar	status;
#define cmd	status
	uchar	u5;
	uchar	reset;
	uchar	u6;
	uchar	send;
	uchar	u7;
};
#define	INCON	((Device *)0x40700000)

struct Incon {
	QLock;

	QLock	xmit;		/* transmit lock */
	QLock	reslock;	/* reset lock */
	Device	*dev;
	int	station;	/* station number */
	int	state;		/* chip state */
	Rendez	r;		/* output process */
	Rendez	kr;		/* input kernel process */
	ushort	chan;		/* current input channel */
	Queue	*rq;		/* read queue */
	uchar	buf[1024];	/* bytes being collected */
	uchar	*wptr;		/* pointer into buf */
	int	kstarted;	/* true if kernel process started */

	ushort	raw[Nraw];
	ushort	*rp;
	ushort	*wp;

	/* statistics */

	ulong	overflow;	/* overflow errors */
	ulong	pack0;		/* channel 0 */
	ulong	crc;		/* crc errors */
	ulong	in;		/* bytes in */
	ulong	out;		/* bytes out */
};

Incon incon[Nincon];

/*
 *  chip state
 */
enum {
	Selecting,
	Selected,
	Dead,
};

/*
 *  internal chip registers
 */
#define	sel_polln	0
#define	sel_station	1
#define	sel_poll0	2
#define sel_rcv_cnt	3
#define sel_rcv_tim	4
#define sel_tx_cnt	5

/*
 *  CSR bits
 */
#define INCON_RUN	0x80
#define INCON_STOP	0x00
#define ENABLE_IRQ	0x40
#define ENABLE_TX_IRQ	0x20
#define INCON_ALIVE	0x80
#define TX_FULL		0x10
#define TX_EMPTY	0x08
#define RCV_EMPTY	0x04
#define OVERFLOW	0x02
#define CRC_ERROR	0x01

/*
 *  polling constants
 */
#define HT_GNOT	0x30
#define ST_UNIX 0x04
#define NCHAN 16

static void inconkproc(void*);

/*
 *  incon stream module definition
 */
static void inconoput(Queue*, Block*);
static void inconstopen(Queue*, Stream*);
static void inconstclose(Queue*);
Qinfo inconinfo = { nullput, inconoput, inconstopen, inconstclose, "incon" };

/*
 *  set the incon parameters
 */
void
inconset(Incon *ip, int cnt, int delay)
{
	Device *dev;

	if (cnt<1 || cnt>14 || delay<1 || delay>15)
		error(0, Ebadarg);

	dev = ip->dev;
	dev->cmd = sel_rcv_cnt | INCON_RUN;
	*(uchar *)&dev->data_cntl = cnt;
	dev->cmd = sel_rcv_tim | INCON_RUN;
	*(uchar *)&dev->data_cntl = delay;
	dev->cmd = INCON_RUN | ENABLE_IRQ;
}

static void
nop(void)
{
}

/*
 *  poll for a station number
 */
void 
inconpoll(Incon *ip, int station)
{
	ulong timer;
	Device *dev;

	dev = ip->dev;

	/*
	 *  get us to a known state
	 */
	ip->state = Dead;
	dev->cmd = INCON_STOP;

	/*
	 * try a station number
	 */
	dev->cmd = sel_station;
	*(uchar *)&dev->data_cntl = station;
	dev->cmd = sel_poll0;
	*(uchar *)&dev->data_cntl = HT_GNOT;
	dev->cmd = sel_rcv_cnt;
	*(uchar *)&dev->data_cntl = 3;
	dev->cmd = sel_rcv_tim;
	*(uchar *)&dev->data_cntl = 15;
	dev->cmd = sel_tx_cnt;
	*(uchar *)&dev->data_cntl = 1;
	dev->cmd = sel_polln;
	*(uchar *)&dev->data_cntl = 0x00;
	*(uchar *)&dev->data_cntl = ST_UNIX;
	*(uchar *)&dev->data_cntl = NCHAN;
	*(uchar *)&dev->data_cntl = 'g';
	*(uchar *)&dev->data_cntl = 'n';
	*(uchar *)&dev->data_cntl = 'o';
	*(uchar *)&dev->data_cntl = 't';
	dev->cpolln = 0;

	/*
	 *  poll and wait for ready (or 1/4 second)
	 */
	ip->state = Selecting;
	dev->cmd = INCON_RUN | ENABLE_IRQ;
	timer = NOW + 250;
	while (NOW < timer) {
		nop();
		if(dev->status & INCON_ALIVE){
			ip->station = station;
			ip->state = Selected;
			break;
		}
	}
}

/*
 *  reset the chip and find a new staion number
 */
void
inconrestart(Incon *ip)
{
	Device *dev;
	int i;

	if(!canqlock(&ip->reslock))
		return;

	/*
	 *  poll for incon station numbers
	 */
	for(i = Minstation; i <= Maxstation; i++){
		inconpoll(ip, i);
		if(ip->state == Selected)
			break;
	}
	switch(ip->state) {
	case Selecting:
		print("incon[%d] not polled\n", ip-incon);
		break;
	case Selected:
		print("incon[%d] station %d\n", ip-incon, ip->station);
		inconset(ip, 8, 9);
		break;
	default:
		print("incon[%d] bollixed\n", ip-incon);
		break;
	}
	qunlock(&ip->reslock);
}

/*
 *  reset all incon chips.
 */
void
inconreset(void)
{
	int i;
	Incon *ip;

	incon[0].dev = INCON;
	incon[0].state = Selected;
	incon[0].rp = incon[0].wp = incon[0].raw;
	for(i=1; i<Nincon; i++){
		incon[i].dev = INCON+i;
		incon[i].state = Dead;
		incon[i].dev->cmd = INCON_STOP;
		incon[i].rp = incon[i].wp = incon[i].raw;
	}
}

void
inconinit(void)
{
}

/*
 *  enable the device for interrupts, spec is the device number
 */
Chan*
inconattach(char *spec)
{
	Incon *ip;
	int i;
	Chan *c;

	i = strtoul(spec, 0, 0);
	if(i >= Nincon)
		error(0, Ebadarg);
	ip = &incon[i];
	if(ip->state != Selected)
		inconrestart(ip);

	c = devattach('i', spec);
	c->dev = i;
	c->qid = CHDIR;
	return c;
}

Chan*
inconclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
inconwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, streamgen);
}

void	 
inconstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, streamgen);
}

Chan*
inconopen(Chan *c, int omode)
{
	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eperm);
	}else
		streamopen(c, &inconinfo);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
inconcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void	 
inconclose(Chan *c)
{
	if(c->qid != CHDIR)
		streamclose(c);
}

long	 
inconread(Chan *c, void *buf, long n)
{
	return streamread(c, buf, n);
}

long	 
inconwrite(Chan *c, void *buf, long n)
{
	return streamwrite(c, buf, n, 0);
}

void	 
inconremove(Chan *c)
{
	error(0, Eperm);
}

void	 
inconwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
inconuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
inconerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

/*
 *	the stream routines
 */

/*
 *  create the kernel process for input
 */
static void
inconstopen(Queue *q, Stream *s)
{
	Incon *ip;
	char name[32];

	ip = &incon[s->dev];
	sprint(name, "**incon%d**", s->dev);
	q->ptr = q->other->ptr = ip;
	ip->rq = q;
	kproc(name, inconkproc, ip);
}

/*
 *  kill off the kernel process
 */
static int
kDead(void *arg)
{
	Incon *ip;

	ip = (Incon *)arg;
	return ip->kstarted == 0;
}
static void
inconstclose(Queue * q)
{
	Incon *ip;

	ip = (Incon *)q->ptr;
	qlock(ip);
	ip->rq = 0;
	qunlock(ip);
	wakeup(&ip->kr);
	sleep(&ip->r, kDead, ip);
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
 *  output a block
 *
 *  the first 2 bytes of every message are the channel number,
 *  low order byte first.  the third is a possible trailing control
 *  character.
 */
void
inconoput(Queue *q, Block *bp)
{
	Device *dev;
	Incon *ip;
	ulong end;
	int chan;
	int ctl;
	int n, size;

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
	ip = (Incon *)q->ptr;
	qlock(&ip->xmit);
	dev = ip->dev;

	/*
	 *  parse message
	 */
	bp = getq(q);
	if(bp->wptr - bp->rptr < 3){
		freemsg(q, bp);
		qunlock(&ip->xmit);
		return;
	}
	chan = bp->rptr[0] | (bp->rptr[1]<<8);
	ctl = bp->rptr[2];
	bp->rptr += 3;

	/*
	 *  make sure there's an incon out there
	 */
	if(!(dev->status&INCON_ALIVE) || ip->state==Dead){
		inconrestart(ip);
		freemsg(q, bp);
		qunlock(&ip->xmit);
		return;
	}

	/*
	 *  send the 8 bit data
	 */
	for(;;){
		/*
		 *  spin till there is room
		 */
		for(end = NOW+1000; dev->status & TX_FULL;){
			nop();	/* make sure we don't optimize too much */
			if(NOW > end){
				print("incon output stuck\n");
				freemsg(q, bp);
				qunlock(&ip->xmit);
				return;
			}
		}

		/*
		 *  put in next packet
		 */
		n = bp->wptr - bp->rptr;
		if(n > 16)
			n = 16;
		size = n;
		dev->cdata = chan;
		DPRINT("CH|%uo->\n", chan);
		while(n--){
			DPRINT("->%uo\n", *bp->rptr);
			*(uchar *)&dev->data_cntl = *bp->rptr++;
		}

		/*
		 *  get next block 
		 */
		if(bp->rptr >= bp->wptr){
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
		 *  end packet
		 */
		dev->cdata = 0;
	}

	/*
	 *  send the control byte if there is one
	 */
	if(ctl){
		if(size >= 16){
			dev->cdata = 0;
			DPRINT("CH|%uo->\n", chan);
			dev->cdata = chan;
		}
		DPRINT("CTL|%uo->\n", ctl);
		dev->cdata = ctl;
	}
	dev->cdata = 0;

	qunlock(&ip->xmit);
	return;
}

/*
 *  return true if the raw fifo is non-empty
 */
static int
notempty(void *arg)
{
	Incon *ip;

	ip = (Incon *)arg;
	return ip->wp!=ip->rp;
}

/*
 *  fill a block with what is currently buffered and send it upstream
 */
static void
upstream(Incon *ip, unsigned int ctl)
{
	int n;
	Block *bp;

	n = ip->wptr - ip->buf;
	bp = allocb(3 + n);
	bp->wptr[0] = ip->chan;
	bp->wptr[1] = ip->chan>>8;
	bp->wptr[2] = ctl;
	if(n)
		memcpy(&bp->wptr[3], ip->buf, n);
	bp->wptr += 3 + n;
	bp->flags |= S_DELIM;
	PUTNEXT(ip->rq, bp);
	ip->wptr = ip->buf;
}

/*
 *  Read bytes from the raw input circular buffer.
 */
static void
inconkproc(void *arg)
{
	Incon *ip;
	unsigned int c;
	uchar *lim;
	ushort *p, *e;

	ip = (Incon *)arg;
	ip->kstarted = 1;
	ip->wptr = ip->buf;

	e = &ip->raw[Nraw];
	for(;;){
		/*
		 *  die if the device is closed
		 */
		qlock(ip);
		if(ip->rq == 0){
			qunlock(ip);
			ip->kstarted = 0;
			wakeup(&ip->r);
			return;
		}

		/*
		 *  sleep if input fifo empty
		 */
		while(!notempty(ip))
			sleep(&ip->kr, notempty, ip);
		p = ip->rp;

		/*
		 *  get channel number
		 */
		c = (*p++)>>8;
		if(p == e)
			p = ip->raw;
		DPRINT("<-CH|%uo\n", c);
		if(ip->chan != c){
			if(ip->wptr - ip->buf != 0)
				upstream(ip, 0);
			ip->chan = c;
		}

		/*
		 *  null byte marks end of packet
		 */
		for(lim = &ip->buf[sizeof ip->buf];;){
			if((c=*p++)&1) {
				/*
				 *  data byte, put in local buffer
				 */
				c = *ip->wptr++ = c>>8;
				DPRINT("<-%uo\n", c);
				if(ip->wptr >= lim)
					upstream(ip, 0);
			} else if (c>>=8) {
				/*
				 *  control byte ends block
				 */
				DPRINT("<-CTL|%uo\n", c);
				upstream(ip, c);
			} else {
				/* end of packet */
				break;
			}
			if(p == e)
				p = ip->raw;
		}
		ip->rp = p;
		qunlock(ip);
	}
}

/*
 *  read the packets from the device into the raw input buffer.
 *  we have to do this at interrupt tevel to turn off the interrupts.
 */
static
rdpackets(Incon *ip)
{
	Device *dev;
	unsigned int c;
	ushort *p, *e;
	int n;

	dev = ip->dev;
	while(!(dev->status & RCV_EMPTY)){
		n = ip->rp - ip->wp;
		if(n <= 0)
			n += Nraw;
		if(n < 19){
			/*
			 *  no room in the raw queue, throw it away
			 */
			c = (dev->data_cntl)>>8;
			for(c=0;c<18;c++){
				if(dev->data_cntl == 0)
					break;
			}
		} else {
			/*
			 *  put packet in the raw queue
			 */
			p = ip->wp;
			e = &ip->raw[Nraw];
			*p++ = dev->data_cntl;
			if(p == e)
				p = ip->raw;
			do {
				*p++ = c = dev->data_cntl;
				if(p == e)
					p = ip->raw;
			} while(c);
			ip->wp = p;
		}
	}
	wakeup(&ip->kr);
}

/*
 *  Receive an incon interrupt.  One entry point
 *  for all types of interrupt.  Until we figure out
 *  how to use more than one incon, this routine only
 *  is for incon[0].
 */
inconintr(Ureg *ur)
{
	uchar status;
	Incon *ip;

	ip = &incon[0];

	status = ip->dev->status;
	if(!(status & RCV_EMPTY))
		rdpackets(ip);

	/* check for exceptional conditions */
	if(status&(OVERFLOW|CRC_ERROR)){
		if(status&OVERFLOW){
			print("incon overflow\n");
			ip->overflow++;
		}
		if(status&CRC_ERROR){
			print("incon crc error\n");
			ip->crc++;
		}
	}

	/* see if it died underneath us */
	if(!(status&INCON_ALIVE)){
		switch(ip->state){
		case Selected:
			ip->dev->cmd = INCON_STOP;
			print("Incon died\n");
			break;
		case Selecting:
			print("rejected\n");
			break;
		default:
			ip->dev->cmd = INCON_STOP;
			break;
		}
		ip->state = Dead;
	}
}
