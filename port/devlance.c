#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"
#include	"devtab.h"

enum {
	Ntypes=		9,		/* max number of ethernet packet types */
	Maxrb=		128,		/* max buffers in a ring */
};
#define RSUCC(x) (((x)+1)%l.nrrb)
#define TSUCC(x) (((x)+1)%l.ntrb)

#define NOW (MACHP(0)->ticks*MS2HZ)

static int netlight;

/*
 *  Communication with the lance is via a transmit and receive ring of
 *  message descriptors.  The Initblock contains pointers to and sizes of
 *  these rings.  The rings must be in RAM addressible by the lance
 */
typedef struct {
	ushort	laddr;		/* low order piece of address */
	ushort	flags;		/* flags and high order piece of address */
	short	size;		/* size of buffer */
	ushort	cntflags;	/* (rcv)count of bytes in buffer; (xmt) more flags */
} Msg;

/*
 *  lance memory map
 */
struct Lancemem
{
	/*
	 *  initialization block
	 */
	ushort	mode;		/* chip control (see below) */
	ushort	etheraddr[3];	/* the ethernet physical address */
	ushort	multi[4];	/* multicast addresses, 1 bit for each of 64 */
	ushort	rdralow;	/* receive buffer ring */
	ushort	rdrahigh;	/* (top three bits define size of ring) */
	ushort	tdralow;	/* transmit buffer ring */
	ushort	tdrahigh;	/* (top three bits define size of ring) */
	
	/*
	 *  ring buffers
	 *  first receive, then transmit
	 */
	Msg	rmr[Maxrb];		/* recieve message ring */
	Msg	tmr[Maxrb];		/* transmit message ring */
};

/*
 *  Some macros for dealing with lance memory addresses.  The lance splits
 *  its 24 bit addresses across two 16 bit registers.
 */
#define HADDR(a) ((((ulong)(a))>>16)&0xFF)
#define LADDR(a) (((ulong)a)&0xFFFF)

/*
 *  The following functions exist to sidestep a quirk in the SGI IO3 lance
 *  interface.  In all other processors, the lance's initialization block and
 *  descriptor rings look like normal memory.  In the SGI IO3, the CPU sees a
 *  6 byte pad twixt all lance memory shorts.  Therefore, we use the following
 *  macros to compute the address whenever accessing the lance memory to make
 *  the code portable.  Sic transit gloria.
 */
#define LANCEMEM ((Lancemem*)0)
#define MPs(a) (*(short *)(l.lanceram + l.sep*((ushort*)&a - (ushort*)0)))
#define MPus(a) (*(ushort *)(l.lanceram + l.sep*((ushort*)&a - (ushort*)0)))

/*
 *  one per ethernet packet type
 */
typedef struct {
	QLock;
	int	type;		/* ethernet type */
	int	prom;		/* promiscuous mode */
	Queue	*q;
	int	inuse;
} Ethertype;


/*
 *  lance state
 */
typedef struct {
	QLock;

	Lance;			/* host dependent lance params */
	int	prom;		/* number of promiscuous channels */
	int	all;		/* number of channels listening to all packets */
	Network	net;
	Netprot	prot[Ntypes];

	int	inited;
	uchar	*lmp;		/* location of parity test */

	Rendez	rr;		/* rendezvous for an input buffer */
	QLock	rlock;		/* semaphore on tc */
	ushort	rl;		/* first rcv Message belonging to Lance */	
	ushort	rc;		/* first rcv Message belonging to CPU */

	Rendez	tr;		/* rendezvous for an output buffer */
	QLock	tlock;		/* semaphore on tc */
	ushort	tl;		/* first xmt Message belonging to Lance */	
	ushort	tc;		/* first xmt Message belonging to CPU */	

	Ethertype e[Ntypes];
	int	debug;
	int	kstarted;
	uchar	bcast[6];

	Queue	self;	/* packets turned around at the interface */

	/* sadistics */

	int	misses;
	int	inpackets;
	int	outpackets;
	int	crcs;		/* input crc errors */
	int	oerrs;		/* output erros */
	int	frames;		/* framing errors */
	int	overflows;	/* packet overflows */
	int	buffs;		/* buffering errors */
} SoftLance;
static SoftLance l;

/*
 *  mode bits in the lance initialization block
 */
#define PROM	0x8000
#define INTL	0x40
#define DRTY	0x20
#define COLL	0x10
#define DTCR	0x8
#define LOOP	0x4
#define DTX	0x2
#define DRX	0x1

/*
 *  LANCE CSR0, this is the register we play with most often.  We leave
 *  this register pointed to by l.rap in normal operation.
 */
#define ERR0	0x8000
#define BABL	0x4000
#define CERR	0x2000
#define MISS	0x1000
#define MERR	0x800
#define RINT	0x400
#define TINT	0x200
#define IDON	0x100
#define INTR	0x80
#define INEA	0x40
#define RXON	0x20
#define TXON	0x10
#define TDMD	0x8
#define STOP	0x4
#define STRT	0x2
#define INIT	0x1

/*
 *  flag bits from a buffer descriptor in the rcv/xmt rings
 */
#define OWN	0x8000	/* 1 means that the buffer can be used by the chip */
#define ERR	0x4000	/* error summary, the OR of all error bits */
#define FRAM	0x2000	/* CRC error and incoming packet not a multiple of 8 bits */
#define OFLO	0x1000	/* (receive) lost some of the packet */
#define MORE	0x1000	/* (transmit) more than 1 retry to send the packet */
#define CRC	0x800	/* (receive) crc error reading packet */
#define ONE	0x800	/* (transmit) one retry to transmit the packet */
#define BUF	0x400	/* (receive) out of buffers while reading a packet */
#define DEF	0x400	/* (transmit) deffered while transmitting packet */
#define STP	0x200	/* start of packet */
#define ENP	0x100	/* end of packet */

/*
 *  cntflags bits from a buffer descriptor in the rcv/xmt rings
 */
#define BUFF	0x8000	/* buffer error (host screwed up?) */
#define UFLO	0x4000	/* underflow from memory */
#define LCOL	0x1000	/* late collision (ether too long?) */
#define LCAR	0x800	/* loss of carrier (ether broken?) */
#define RTRY	0x400	/* couldn't transmit (bad station on ether?) */
#define TTDR	0x3FF	/* time domain reflectometer */

/*
 *  predeclared
 */
static void	lancekproc(void *);
static void	lancestart(int, int);
static void	lanceup(Etherpkt*, int);
static int	lanceclonecon(Chan*);
static void	lancestatsfill(Chan*, char*, int);
static void	lancetypefill(Chan*, char*, int);

/*
 *  lance stream module definition
 */
static void lanceoput(Queue*, Block*);
static void lancestopen(Queue*, Stream*);
static void lancestclose(Queue*);
static void stagerbuf(void);
Qinfo lanceinfo = { nullput, lanceoput, lancestopen, lancestclose, "lance" };

/*
 *  open a lance line discipline
 *
 *  the lock is to synchronize changing the ethertype with
 *  sending packets up the stream on interrupts.
 */
void
lancestopen(Queue *q, Stream *s)
{
	Ethertype *et;

	et = &l.e[s->id];
	qlock(et);
	RD(q)->ptr = WR(q)->ptr = et;
	et->type = 0;
	et->q = RD(q);
	et->inuse = 1;
	qunlock(et);
}

/*
 *  close lance line discipline
 *
 *  the lock is to synchronize changing the ethertype with
 *  sending packets up the stream on interrupts.
 */
static void
lancestclose(Queue *q)
{
	Ethertype *et;

	et = (Ethertype *)(q->ptr);
	if(et->prom){
		qlock(&l);
		l.prom--;
		if(l.prom == 0)
			lancestart(0, 1);
		qunlock(&l);
	}
	if(et->type == -1){
		qlock(&l);
		l.all--;
		qunlock(&l);
	}
	qlock(et);
	et->type = 0;
	et->q = 0;
	et->prom = 0;
	et->inuse = 0;
	netdisown(&l.net, et - l.e);
	qunlock(et);
}

/*
 *  the ``connect'' control message specifyies the type
 */
Proc *lanceout;
static int
isobuf(void *x)
{
	USED(x);
	return TSUCC(l.tc) != l.tl;
}
static void
lanceoput(Queue *q, Block *bp )
{
	int n, len;
	Etherpkt *p;
	Ethertype *e;
	Msg *m;
	Block *nbp;

	if(bp->type == M_CTL){
		e = q->ptr;
		qlock(&l);
		if(streamparse("connect", bp)){
			if(e->type == -1)
				l.all--;
			e->type  = strtol((char *)bp->rptr, 0, 0);
			if(e->type == -1)
				l.all++;
		} else if(streamparse("promiscuous", bp)) {
			e->prom = 1;
			l.prom++;
			if(l.prom == 1)
				lancestart(PROM, 1);/**/
		}
		qunlock(&l);
		freeb(bp);
		return;
	}

	/*
	 *  give packet a local address, return upstream if destined for
	 *  this machine.
	 */
	if(BLEN(bp) < ETHERHDRSIZE){
		bp = pullup(bp, ETHERHDRSIZE);
		if(bp == 0)
			return;
	}
	p = (Etherpkt *)bp->rptr;
	memmove(p->s, l.ea, sizeof(l.ea));
	if(memcmp(l.ea, p->d, sizeof(l.ea)) == 0){
		len = blen(bp);
		bp = expandb(bp, len >= ETHERMINTU ? len : ETHERMINTU);
		if(bp){
			putq(&l.self, bp);
			wakeup(&l.rr);
		}
		return;
	}
	if(memcmp(l.bcast, p->d, sizeof(l.bcast)) == 0 || l.prom || l.all){
		len = blen(bp);
		nbp = copyb(bp, len);
		nbp = expandb(nbp, len >= ETHERMINTU ? len : ETHERMINTU);
		if(nbp){
			nbp->wptr = nbp->rptr+len;
			putq(&l.self, nbp);
			wakeup(&l.rr);
		}
	}

	/*
	 *  only one transmitter at a time
	 */
	qlock(&l.tlock);

	if(waserror()){
		freeb(bp);
		qunlock(&l.tlock);
		nexterror();
	}

	/*
	 *  Wait till we get an output buffer, reset the lance if input
	 *  or output seems wedged.
	 */
	tsleep(&l.tr, isobuf, (void *)0, 1000);
	if(isobuf(0) == 0 || l.misses > 4){
		l.misses = 0;
		print("lance wedged, restarting\n");
		lancestart(0, 0);
		qunlock(&l.tlock);
		freeb(bp);
		return;		/* the interrupt routine will qunlock(&l.tlock) */
	}
	p = &l.tp[l.tc];

	/*
	 *  copy message into lance RAM
	 */
	len = 0;
	for(nbp = bp; nbp; nbp = nbp->next){
		if(sizeof(Etherpkt) - len >= (n = BLEN(nbp))){
			memmove(((uchar *)p)+len, nbp->rptr, n);
			len += n;
		} else
			print("no room damn it\n");
		if(bp->flags & S_DELIM)
			break;
	}

	/*
	 *  pad the packet (zero the pad)
	 */
	if(len < ETHERMINTU){
		memset(((char*)p)+len, 0, ETHERMINTU-len);
		len = ETHERMINTU;
	}

	/*
	 *  set up the ring descriptor and hand to lance
	 */
	l.outpackets++;
	m = &(LANCEMEM->tmr[l.tc]);
	MPs(m->size) = -len;
	MPus(m->cntflags) = 0;
	MPus(m->laddr) = LADDR(&l.ltp[l.tc]);
	MPus(m->flags) = OWN|STP|ENP|HADDR(&l.ltp[l.tc]);
	l.tc = TSUCC(l.tc);
	*l.rdp = INEA|TDMD; /**/
	wbflush();
	freeb(bp);
	qunlock(&l.tlock);
	poperror();
}

/*
 *  stop the lance and allocate buffers
 */
void
lancereset(void)
{
	static int already;

	if(already == 0){
		already = 1;
		lancesetup(&l);

		l.net.name = "ether";
		l.net.nconv = Ntypes;
		l.net.devp = &lanceinfo;
		l.net.protop = 0;
		l.net.listen = 0;
		l.net.clone = lanceclonecon;
		l.net.ninfo = 2;
		l.net.prot = l.prot;
		l.net.info[0].name = "stats";
		l.net.info[0].fill = lancestatsfill;
		l.net.info[1].name = "type";
		l.net.info[1].fill = lancetypefill;

		memset(l.bcast, 0xff, sizeof l.bcast);
	}

	/*
	 *  stop the lance
	 */
	*l.rap = 0;
	*l.rdp = STOP;
}

/*
 *  Initialize and start the lance.  This routine can be called at any time.
 *  It may be used to restart a dead lance.
 */
static void
lancestart(int mode, int dolock)
{
	int i;
	Etherpkt *p;
	Lancemem *lm = LANCEMEM;
	Msg *m;

	/*
	 *   wait till both receiver and transmitter are
	 *   quiescent
	 */
	if(dolock)
		qlock(&l.tlock);
	qlock(&l.rlock);

	lancereset();
	l.rl = 0;
	l.rc = 0;
	l.tl = 0;
	l.tc = 0;

	/*
	 *  create the initialization block
	 */
	MPus(lm->mode) = mode;

	/*
	 *  set ether addr from the value in the id prom.
	 *  the id prom has them in reverse order, the init
	 *  structure wants them in byte swapped order
	 */
	MPus(lm->etheraddr[0]) = (l.ea[1]<<8) | l.ea[0];
	MPus(lm->etheraddr[1]) = (l.ea[3]<<8) | l.ea[2];
	MPus(lm->etheraddr[2]) = (l.ea[5]<<8) | l.ea[4];

	/*
	 *  ignore multicast addresses
	 */
	MPus(lm->multi[0]) = 0;
	MPus(lm->multi[1]) = 0;
	MPus(lm->multi[2]) = 0;
	MPus(lm->multi[3]) = 0;

	/*
	 *  set up rcv message ring
	 */
	m = lm->rmr;
	for(i = 0; i < l.nrrb; i++, m++){
		MPs(m->size) = -sizeof(Etherpkt);
		MPus(m->cntflags) = 0;
		MPus(m->laddr) = LADDR(&l.lrp[i]);
		MPus(m->flags) = HADDR(&l.lrp[i]);
	}
	MPus(lm->rdralow) = LADDR(l.lm->rmr);
	MPus(lm->rdrahigh) = (l.lognrrb<<13)|HADDR(l.lm->rmr);


	/*
	 *  give the lance all the rcv buffers except one (as a sentinel)
	 */
	l.rc = l.nrrb - 1;
	m = lm->rmr;
	for(i = 0; i < l.rc; i++, m++)
		MPus(m->flags) |= OWN;

	/*
	 *  set up xmit message ring
	 */
	m = lm->tmr;
	for(i = 0; i < l.ntrb; i++, m++){
		MPs(m->size) = 0;
		MPus(m->cntflags) = 0;
		MPus(m->laddr) = LADDR(&l.ltp[i]);
		MPus(m->flags) = HADDR(&l.ltp[i]);
	}
	MPus(lm->tdralow) = LADDR(l.lm->tmr);
	MPus(lm->tdrahigh) = (l.logntrb<<13)|HADDR(l.lm->tmr);

	/*
	 *  point lance to the initialization block
	 */
	*l.rap = 1;
	*l.rdp = LADDR(l.lm);
	wbflush();
	*l.rap = 2;
	*l.rdp = HADDR(l.lm);

	/*
	 *  The lance byte swaps the ethernet packet unless we tell it not to
	 */
	wbflush();
	*l.rap = 3;
	*l.rdp = l.busctl;

	/*
	 *  initialize lance, turn on interrupts, turn on transmit and rcv.
	 */
	wbflush();
	*l.rap = 0;
	*l.rdp = INEA|INIT|STRT; /**/
}

/*
 *  set up lance directory.
 */
void
lanceinit(void)
{
}

Chan*
lanceattach(char *spec)
{
	if(l.kstarted == 0){
		kproc("lancekproc", lancekproc, 0);/**/
		l.kstarted = 1;
		lancestart(0, 1);
	}
	return devattach('l', spec);
}

Chan*
lanceclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
lancewalk(Chan *c, char *name)
{
	return netwalk(c, name, &l.net);
}

void	 
lancestat(Chan *c, char *dp)
{
	netstat(c, dp, &l.net);
}

/*
 *  Pass open's of anything except the directory to streamopen
 */
Chan*
lanceopen(Chan *c, int omode)
{
	return netopen(c, omode, &l.net);
}

void	 
lancecreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void	 
lanceclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long	 
lanceread(Chan *c, void *a, long n, ulong offset)
{
	return netread(c, a, n, offset,  &l.net);
}

long	 
lancewrite(Chan *c, void *a, long n, ulong offset)
{
	return streamwrite(c, a, n, 0);
}

void	 
lanceremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void	 
lancewstat(Chan *c, char *dp)
{
	netwstat(c, dp, &l.net);
}

/*
 *  user level network interface routines
 */
static void
lancestatsfill(Chan *c, char* p, int n)
{
	char buf[256];

	USED(c);
	sprint(buf, "in: %d\nout: %d\ncrc errs %d\noverflows: %d\nframe errs %d\nbuff errs: %d\noerrs %d\naddr: %.02x:%.02x:%.02x:%.02x:%.02x:%.02x\n",
		l.inpackets, l.outpackets, l.crcs,
		l.overflows, l.frames, l.buffs, l.oerrs,
		l.ea[0], l.ea[1], l.ea[2], l.ea[3], l.ea[4], l.ea[5]);
	strncpy(p, buf, n);
}

static void
lancetypefill(Chan *c, char* p, int n)
{
	char buf[16];
	Ethertype *e;

	e = &l.e[STREAMID(c->qid.path)];
	sprint(buf, "%d", e->type);
	strncpy(p, buf, n);
}

static int
lanceclonecon(Chan *c)
{
	Ethertype *e;

	USED(c);
	for(e = l.e; e < &l.e[Ntypes]; e++){
		qlock(e);
		if(e->inuse || e->q){
			qunlock(e);
			continue;
		}
		e->inuse = 1;
		netown(&l.net, e - l.e, u->p->user, 0);
		qunlock(e);
		return e - l.e;
	}
	error(Enodev);
}

/*
 *  We will:
 *	(1) Clear interrupt cause in the lance
 *	(2) service all current events
 */
void
lanceintr(void)
{
	int i;
	ushort csr;
	Lancemem *lm = LANCEMEM;

	csr = *l.rdp;

	/*
	 *  turn off the interrupt and any error indicators
	 */
	*l.rdp = IDON|INEA|TINT|RINT|BABL|CERR|MISS|MERR;

	/*
	 *  see if an error occurred
	 */
	if(csr & (BABL|MISS|MERR)){
		if(l.misses++ < 4)
			print("lance err %ux\n", csr);
	}

	if(csr & IDON){
		l.inited = 1;
		qunlock(&l.rlock);
		qunlock(&l.tlock);
	}

	/*
	 *  look for rcv'd packets, just wakeup the input process
	 */
	if(l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0)
		wakeup(&l.rr);

	/*
	 *  look for xmitt'd packets, wake any process waiting for a
	 *  transmit buffer
	 */
	while(l.tl!=l.tc && (MPus(lm->tmr[l.tl].flags) & OWN)==0){
		if(MPus(lm->tmr[l.tl].flags) & ERR)
			l.oerrs++;
		l.tl = TSUCC(l.tl);
		wakeup(&l.tr);
	}
}

/*
 *  send a packet upstream
 */
static void
lanceup(Etherpkt *p, int len)
{
	Block *bp;
	Ethertype *e;
	int t;

	t = (p->type[0]<<8) | p->type[1];
	for(e = &l.e[0]; e < &l.e[Ntypes]; e++){
		/*
		 *  check before locking just to save a lock
		 */
		if(e->q==0 || (t!=e->type && e->type!=-1))
			continue;

		/*
		 *  only a trace channel gets packets destined for other machines
		 */
		if(e->type!=-1 && p->d[0]!=0xff && memcmp(p->d, l.ea, sizeof(p->d))!=0)
			continue;

		/*
		 *  check after locking to make sure things didn't
		 *  change under foot
		 */
		if(!canqlock(e))
			continue;

		if(e->q==0 || e->q->next->len>Streamhi || (t!=e->type && e->type!=-1)){
			qunlock(e);
			continue;
		}

		if(!waserror()){
			bp = allocb(len);
			memmove(bp->rptr, (uchar *)p, len);
			bp->wptr += len;
			bp->flags |= S_DELIM;
			PUTNEXT(e->q, bp);
		}
		poperror();
		qunlock(e);
	}
}

/*
 *  input process, awakened on each interrupt with rcv buffers filled
 */
static int
isinput(void *arg)
{
	Lancemem *lm = LANCEMEM;

	USED(arg);
	return l.self.first || (l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0);
}

static void
lancekproc(void *arg)
{
	Etherpkt *p;
	Ethertype *e;
	int len;
	int t;
	Lancemem *lm = LANCEMEM;
	Msg *m;
	Block *bp;

	USED(arg);
	for(;;){
		qlock(&l.rlock);
		while(bp = getq(&l.self)){
			lanceup((Etherpkt*)bp->rptr, BLEN(bp));
			freeb(bp);
		}
		for(; l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0 ; l.rl=RSUCC(l.rl)){
			l.inpackets++;
			l.misses = 0;
			m = &(lm->rmr[l.rl]);
			t = MPus(m->flags);
			if(t & ERR){
				if(t & FRAM)
					l.frames++;
				if(t & OFLO)
					l.overflows++;
				if(t & CRC)
					l.crcs++;
				if(t & BUFF)
					l.buffs++;
				goto stage;
			}
	
			/*
			 *  stuff packet up each queue that wants it
			 */
			p = &l.rp[l.rl];
			len = MPus(m->cntflags) - 4;
			lanceup(p, len);

stage:
			/*
			 *  stage the next input buffer
			 */
			m = &(lm->rmr[l.rc]);
			MPs(m->size) = -sizeof(Etherpkt);
			MPus(m->cntflags) = 0;
			MPus(m->laddr) = LADDR(&l.lrp[l.rc]);
			MPus(m->flags) = OWN|HADDR(&l.lrp[l.rc]);
			l.rc = RSUCC(l.rc);
			wbflush();
		}
		qunlock(&l.rlock);
		sleep(&l.rr, isinput, 0);
	}
}
