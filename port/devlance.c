#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"devtab.h"

enum {
	Ntypes=		8,		/* max number of ethernet packet types */
	Ndir=		Ntypes+2,	/* entries in top level directory */
	Ndpkt=		200,		/* number of debug packets */
	Maxrb=		128,		/* max buffers in a ring */
};
#define RSUCC(x) (((x)+1)%l.nrrb)
#define TSUCC(x) (((x)+1)%l.ntrb)

#define NOW (MACHP(0)->ticks*MS2HZ)

int plance;

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
	Queue	*q;
} Ethertype;

/*
 *  circular debug queue (first 44 bytes of the last Ndpkt packets)
 */
typedef struct {
	uchar d[6];
	uchar s[6];
	uchar type[2];
	uchar data[60];
} Dpkt;
typedef struct {
	ulong	ticks;
	char	tag;
	int	len;
	Dpkt	p;
} Trace;
typedef struct {
	Lock;
	int	next;
	Trace	t[Ndpkt];
} Debqueue;

/*
 *  lance state
 */
typedef struct {
	QLock;

	Lance;			/* host dependent lance params */

	int	inited;
	uchar	*lmp;		/* location of parity test */

	Rendez	rr;		/* rendezvous for an input buffer */
	ushort	rl;		/* first rcv Message belonging to Lance */	
	ushort	rc;		/* first rcv Message belonging to CPU */

	Rendez	tr;		/* rendezvous for an output buffer */
	QLock	tlock;		/* semaphore on tc */
	ushort	tl;		/* first xmt Message belonging to Lance */	
	ushort	tc;		/* first xmt Message belonging to CPU */	

	Ethertype e[Ntypes];
	int	debug;
	int	kstarted;
	Debqueue dq;

	/* sadistics */

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
void lancekproc(void *);

/*
 *  print a packet preceded by a message
 */
sprintpacket(char *buf, Trace *t)
{
	Dpkt *p = &t->p;
	int i;

	sprint(buf, "%c: %.8ud %.4d d(%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux)s(%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux)t(%.2ux %.2ux)d(",
		t->tag, t->ticks, t->len,
		p->d[0], p->d[1], p->d[2], p->d[3], p->d[4], p->d[5],
		p->s[0], p->s[1], p->s[2], p->s[3], p->s[4], p->s[5],
		p->type[0], p->type[1]);
	for(i=0; i<sizeof(p->data); i++)
		sprint(buf+strlen(buf), "%.2ux", p->data[i]);
	sprint(buf+strlen(buf), ")\n");
}

/*
 *  save a message in a circular queue for later debugging
 */
void
lancedebq(char tag, Etherpkt *p, int len)
{
	Trace *t;

	lock(&l.dq);
	t = &l.dq.t[l.dq.next];
	t->ticks = NOW;
	t->tag = tag;
	t->len = len;
	memcpy(&(t->p), p, sizeof(Dpkt));
	l.dq.next = (l.dq.next+1) % Ndpkt;
	unlock(&l.dq);
	if(plance){
		char buf[1024];
		if(p->d[0] != 0xff){
			sprintpacket(buf, t);
			print("%s\n", buf);
		}
	} /**/
}

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
	qlock(et);
	et->type = 0;
	et->q = 0;
	qunlock(et);
}

/*
 *  assume the q is locked external to this routine
 *
 *  the ``connect'' control message specifyies the type
 */
Proc *lanceout;
static int
isobuf(void *x)
{
	return TSUCC(l.tc) != l.tl;
}
static void
lanceoput(Queue *q, Block *bp )
{
	int n, len;
	Etherpkt *p;
	Msg *m;

	if(bp->type == M_CTL){
		if(streamparse("connect", bp)){
			((Ethertype *)q->ptr)->type = strtoul((char *)bp->rptr, 0, 0);
		}
		freeb(bp);
		return;
	}

	/*
	 *  save up to a delim
	 */
	if(!putq(q, bp))
		return;

	/*
	 *  only one transmitter at a time
	 */
	qlock(&l.tlock);

	/*
	 *  Wait till we get an output buffer
	 */
	if(TSUCC(l.tc) == l.tl){
		sleep(&l.tr, isobuf, (void *)0);
	}
	p = &l.tp[l.tc];

	/*
	 *  copy message into lance RAM
	 */
	len = 0;
	while(bp = getq(q)){
		if(sizeof(Etherpkt) - len >= (n = BLEN(bp))){
			memcpy(((uchar *)p)+len, bp->rptr, n);
			len += n;
		} else
			print("no room damn it\n");
		if(bp->flags & S_DELIM){
			freeb(bp);
			break;
		} else
			freeb(bp);
	}

	/*
	 *  give packet a local address
	 */
	memcpy(p->s, l.ea, sizeof(l.ea));

	/*
	 *  pad the packet (zero the pad)
	 */
	if(len < 60){
		memset(((char*)p)+len, 0, 60-len);
		len = 60;
	}

	lancedebq('o', p, len);/**/

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
	qunlock(&l.tlock);
}

/*
 *  lance directory
 */
enum {
	Lchanqid = 1,
	Ltraceqid = 2,
	Lstatsqid = 3,
};
Dirtab lancedir[Ndir];

/*
 *  stop the lance and allocate buffers
 */
void
lancereset(void)
{
	int i;
	ulong x;
	int index;
	static int already;
	ushort *lanceaddr;
	ushort *hostaddr;


	if(already == 0){
		already = 1;
		lancesetup(&l);
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
lancestart(void)
{
	int i;
	Etherpkt *p;
	Lancemem *lm = LANCEMEM;
	Msg *m;

	lancereset();

	/*
	 *  create the initialization block
	 */
	MPus(lm->mode) = 0;

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
 *  set up the free list and lance directory.
 *  start the lance.
 */
void
lanceinit(void)
{
	int i;

	/*
	 *  staticly set up types for now
	 */
	for(i=0; i<Ntypes; i++) {
		sprint(lancedir[i].name, "%d", i);
		lancedir[i].qid.path = CHDIR|STREAMQID(i, Lchanqid);
		lancedir[i].qid.vers = 0;
		lancedir[i].length = 0;
		lancedir[i].perm = 0600;
	}
	strcpy(lancedir[Ntypes].name, "trace");
	lancedir[Ntypes].qid.path = Ltraceqid;
	lancedir[Ntypes].length = 0;
	lancedir[Ntypes].perm = 0600;
	strcpy(lancedir[Ntypes+1].name, "stats");
	lancedir[Ntypes+1].qid.path = Lstatsqid;
	lancedir[Ntypes+1].length = 0;
	lancedir[Ntypes+1].perm = 0600;
}

Chan*
lanceattach(char *spec)
{
	Chan *c;

	if(l.kstarted == 0){
		kproc("lancekproc", lancekproc, 0);/**/
		l.kstarted = 1;
		lancestart();
	}
	c = devattach('l', spec);
	c->dev = 0;
	return c;
}

Chan*
lanceclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

/*
 *  if the name doesn't exist, the name is numeric, and room exists
 *  in lancedir, create a new entry.
 */
int	 
lancewalk(Chan *c, char *name)
{
	if(c->qid.path == CHDIR)
		return devwalk(c, name, lancedir, Ndir, devgen);
	else
		return devwalk(c, name, 0, 0, streamgen);
}

void	 
lancestat(Chan *c, char *dp)
{
	if(c->qid.path==CHDIR || c->qid.path==Ltraceqid || c->qid.path==Lstatsqid)
		devstat(c, dp, lancedir, Ndir, devgen);
	else
		devstat(c, dp, 0, 0, streamgen);
}

/*
 *  Pass open's of anything except the directory to streamopen
 */
Chan*
lanceopen(Chan *c, int omode)
{
	extern Qinfo nonetinfo;

	switch(c->qid.path){
	case CHDIR:
	case Ltraceqid:
	case Lstatsqid:
		if(omode != OREAD)
			error(Eperm);
		break;
	default:
		streamopen(c, &lanceinfo);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
lancecreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
lanceclose(Chan *c)
{
	/* real closing happens in lancestclose */
	switch(c->qid.path){
	case CHDIR:
	case Ltraceqid:
	case Lstatsqid:
		break;
	default:
		streamclose(c);
		break;
	}
}

static long
lancetraceread(Chan *c, void *a, long n)
{
	char buf[1024];
	long rv;
	int i;
	char *ca = a;
	int offset;
	Trace *t;
	int plen;

	rv = 0;
	sprintpacket(buf, l.dq.t);
	plen = strlen(buf);
	offset = c->offset % plen;
	for(t = &l.dq.t[c->offset/plen]; n && t < &l.dq.t[Ndpkt]; t++){
		if(t->tag == 0)
			break;
		lock(&l.dq);
		sprintpacket(buf, t);
		unlock(&l.dq);
		i = plen - offset;
		if(i > n)
			i = n;
		memcpy(ca, buf + offset, i);
		n -= i;
		ca += i;
		rv += i;
		offset = 0;
	}
	return rv;
}

long	 
lanceread(Chan *c, void *a, long n)
{
	char buf[256];

	switch(c->qid.path){
	case CHDIR:
		return devdirread(c, a, n, lancedir, Ndir, devgen);
	case Lstatsqid:
		sprint(buf, "in: %d\nout: %d\ncrc errs %d\noverflows: %d\nframe errs %d\nbuff errs: %d\noerrs %d\n",
			l.inpackets, l.outpackets, l.crcs, l.overflows, l.frames,
			l.buffs, l.oerrs);
		return stringread(c, a, n, buf);
	case Ltraceqid:
		return lancetraceread(c, a, n);
	default:
		return streamread(c, a, n);
	}
}

long	 
lancewrite(Chan *c, void *a, long n)
{
	return streamwrite(c, a, n, 0);
}

void	 
lanceremove(Chan *c)
{
	error(Eperm);
}

void	 
lancewstat(Chan *c, char *dp)
{
	error(Eperm);
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
		print("lance err %ux\n", csr);
	}

	if(csr & IDON){
		print("lance inited\n");
		l.inited = 1;
	}

	/*
	 *  look for rcv'd packets, just wakeup the input process
	 */
	if(l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0){
		wakeup(&l.rr);
	}

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
 *  input process, awakened on each interrupt with rcv buffers filled
 */
static int
isinput(void *arg)
{
	Lancemem *lm = LANCEMEM;
	return l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0;
}
void
lancekproc(void *arg)
{
	Block *bp;
	Etherpkt *p;
	Ethertype *e;
	int t;
	int len;
	int i, last;
	Lancemem *lm = LANCEMEM;
	Msg *m;

	for(;;){
		for(; l.rl!=l.rc && (MPus(lm->rmr[l.rl].flags) & OWN)==0 ; l.rl=RSUCC(l.rl)){
			l.inpackets++;
			m = &(lm->rmr[l.rl]);
			if(MPus(m->flags) & ERR){
				t = MPus(m->flags);
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
			 *  See if a queue exists for this packet type.
			 */
			p = &l.rp[l.rl];
			t = (p->type[0]<<8) | p->type[1];
			len = MPus(m->cntflags) - 4;
			lancedebq('i', p, len);/**/
			for(e = &l.e[0]; e < &l.e[Ntypes]; e++){
				if(!canqlock(e))
					continue;
				if(e->q && t == e->type)
					break;
				qunlock(e);
			}
	
			/*
			 *  If no match, see if any stream has type -1.
			 *  It matches all packets.
			 */
			if(e == &l.e[Ntypes]){
				for(e = &l.e[0]; e < &l.e[Ntypes]; e++){
					if(!canqlock(e))
						continue;
					if(e->q && e->type == -1)
						break;
					qunlock(e);
				}
			}
			if(e!=&l.e[Ntypes] && e->q->next->len<=Streamhi){
				/*
				 *  The lock on e makes sure the queue is still there.
				 */
				if(!waserror()){
					bp = allocb(len);
					memcpy(bp->rptr, (uchar *)p, len);
					bp->wptr += len;
					bp->flags |= S_DELIM;
					PUTNEXT(e->q, bp);
					poperror();
				}
				qunlock(e);
			}
	
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
		sleep(&l.rr, isinput, 0);
	}
}

void
lancetoggle(void)
{
	plance ^= 1;
}
