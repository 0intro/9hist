#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"devtab.h"
/*
 *  configuration parameters
 */
enum {
	Ntypes=		8,		/* max number of ethernet packet types */
	Ndir=		Ntypes+1,	/* entries in top level directory */
	LogNrrb=	7,		/* log of number of receive buffers */
	Nrrb=		(1<<LogNrrb),	/* number of recieve buffers */
	LogNtrb=	7,		/* log of number of transmit buffers */
	Ntrb=		(1<<LogNtrb),	/* number of transmit buffers */
	Ndpkt=		200,		/* number of debug packets */
};
#define RSUCC(x) (((x)+1)%Nrrb)
#define TSUCC(x) (((x)+1)%Ntrb)

#define NOW (MACHP(0)->ticks*MS2HZ)

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
 *  Ethernet packet buffers.  These must also be in lance addressible RAM.
 */
typedef struct {
	uchar d[6];
	uchar s[6];
	uchar type[2];
	uchar data[1500];
	uchar crc[4];
} Pkt;

/*
 *  lance memory map
 */
typedef
struct
{
	/*
	 *  initialization block
	 */
	struct initblock {	
		ushort	mode;		/* chip control (see below) */
		ushort	etheraddr[3];	/* the ethernet physical address */
		ushort	multi[4];	/* multicast addresses, 1 bit for each of 64 */
		ushort	rdralow;	/* receive buffer ring */
		ushort	rdrahigh;	/* (top three bits define size of ring) */
		ushort	tdralow;	/* transmit buffer ring */
		ushort	tdrahigh;	/* (top three bits define size of ring) */
	};
	
	/*
	 * ring buffers
	 * first receive, then transmit
	 */
	Msg	rmr[Nrrb];		/* recieve message ring */
	Msg	tmr[Ntrb];		/* transmit message ring */

	/*
	 * actual packets
	 */
	Pkt	p[1];
} Lancemem;
#define LANCEMEM ((Lancemem *)LANCERAM)

/*
 *  Some macros for dealing with lance memory addresses.  The lance splits
 *  its 24 bit addresses across two 16 bit registers.
 */
#define HADDR(a) ((((ulong)(a))>>16)&0xF)
#define LADDR(a) (((ulong)a)&0xFFFF)

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

	int	inited;
	uchar	ea[6];		/* our ether addr */
	uchar	*lmp;		/* location of parity test */
	ushort	*rap;		/* lance address register */
	ushort	*rdp;		/* lance data register */

	Rendez	rr;		/* rendezvous for an input buffer */
	ushort	rl;		/* first rcv Message belonging to Lance */	
	ushort	rc;		/* first rcv Message belonging to CPU */
	Pkt	*rp[Nrrb];	/* receive buffers */
	int	inpackets;

	Rendez	tr;		/* rendezvous for an output buffer */
	QLock	tlock;		/* semaphore on tc */
	ushort	tl;		/* first xmt Message belonging to Lance */	
	ushort	tc;		/* first xmt Message belonging to CPU */	
	Pkt	*tp[Ntrb];	/* transmit buffers */
	int	outpackets;

	Ethertype e[Ntypes];
	int	debug;
	int	kstarted;
	Debqueue dq;
} Lance;
static Lance l;

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
 *  LANCE CSR3
 */
#define BSWP	0x4
#define ACON	0x2
#define BCON	0x1

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
	for(i=0; i<41; i++)
		sprint(buf+strlen(buf), "%.2ux", p->data[i]);
	sprint(buf+strlen(buf), ")\n");
}

/*
 *  save a message in a circular queue for later debugging
 */
void
lancedebq(char tag, Pkt *p, int len)
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
/*	{
		char buf[1024];
		if(p->d[0] != 0xff){
			sprintpacket(buf, t);
			print("%s\n", buf);
		}
	} /**/
}

/*
 *  copy to/from lance memory till we get it right
 */
void
slowcpy(uchar *to, uchar *from, int n)
{
	memcpy(to, from, n);
	while(memcmp(to, from, n)!=0){
		print("lance compare error\n");
		memcpy(to, from, n);
	}
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

	qlock(et);
	et = (Ethertype *)(q->ptr);
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
	Pkt *p;
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
		print("lance obuf sleep");
		sleep(&l.tr, isobuf, (void *)0);
		print("done");
	}
	p = l.tp[l.tc];

	/*
	 *  copy message into lance RAM
	 */
	len = 0;
	while(bp = getq(q)){
		if(sizeof(Pkt) - len >= (n = BLEN(bp))){
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
	 *  pad the packet
	 */
	if(len < 60)
		len = 60;

	lancedebq('o', p, len);

	/*
	 *  set up the ring descriptor and hand to lance
	 */
	m = &(LANCEMEM->tmr[l.tc]);
	m->size = -len;
	m->cntflags = 0;
	m->laddr = LADDR(l.tp[l.tc]);
	m->flags = OWN|STP|ENP|HADDR(l.tp[l.tc]);
	l.tc = TSUCC(l.tc);
	*l.rdp = INEA|TDMD; /**/
	qunlock(&l.tlock);
}

/*
 *  lance directory
 */
enum {
	Lchanqid = 1,
	Ltraceqid = 2,
};
Dirtab lancedir[Ndir];


/*
 *  stop the lance, disable all ring buffers, and free all staged rcv buffers
 */
void
lancereset(void)
{
	Lancemem *lm=LANCEMEM;
	int i;

	/*
	 *  toggle lance's reset line
	 */
	MODEREG->promenet &= ~1;
	MODEREG->promenet |= 1;

	/*
	 *  disable all ring entries
	 */
	l.tl = l.tc = 0;
	for(i = 0; i < Ntrb; i++)
		lm->tmr[i].flags = 0;
	l.rl = l.rc = 0;
	for(i = 0; i < Ntrb; i++)
		lm->rmr[i].flags = 0;

	/*
	 *  run through all lance memory to set parity
	 */
	for(l.lmp=LANCERAM; l.lmp<=LANCEEND; l.lmp++)
		*l.lmp = 55;
}

/*
 *  Initialize and start the lance.  This routine can be called at any time.
 *  It may be used to restart a dead lance.
 */
static void
lancestart(void)
{
	Lancemem *lm=LANCEMEM;
	int i;
	Pkt *p;

	lancereset();

	/*
	 *  create the initialization block
	 */
	lm->mode = 0;

	/*
	 *  set ether addr from the value in the id prom.
	 *  the id prom has them in reverse order, the init
	 *  structure wants them in byte swapped order
	 */
	lm->etheraddr[0] = (LANCEID[16]&0xff00)|((LANCEID[20]>>8)&0xff);
	lm->etheraddr[1] = (LANCEID[8]&0xff00)|((LANCEID[12]>>8)&0xff);
	lm->etheraddr[2] = (LANCEID[0]&0xff00)|((LANCEID[4]>>8)&0xff);
	l.ea[0] = LANCEID[20]>>8;
	l.ea[1] = LANCEID[16]>>8;
	l.ea[2] = LANCEID[12]>>8;
	l.ea[3] = LANCEID[8]>>8;
	l.ea[4] = LANCEID[4]>>8;
	l.ea[5] = LANCEID[0]>>8;
/*
	print("lance addr = %.4ux %.4ux %.4ux\n", lm->etheraddr[0], lm->etheraddr[1],
		lm->etheraddr[2]);
/**/

	/*
	 *  ignore multicast addresses
	 */
	for(i=0; i<4; i++)
		lm->multi[i] = 0;

	/*
	 *  set up rcv message ring
	 */
	p = lm->p;
	for(i = 0; i < Nrrb; i++){
		l.rp[i] = p++;
		lm->rmr[i].size = -sizeof(Pkt);
		lm->rmr[i].cntflags = 0;
		lm->rmr[i].laddr = LADDR(l.rp[i]);
		lm->rmr[i].flags = HADDR(l.rp[i]);
	}
	lm->rdralow = LADDR(lm->rmr);
	lm->rdrahigh = (LogNrrb<<13)|HADDR(lm->rmr);

	/*
	 *  give the lance all the rcv buffers except one (as a sentinel)
	 */
	l.rc = Nrrb - 1;
	for(i = 0; i < l.rc; i++)
		lm->rmr[i].flags |= OWN;

	/*
	 *  set up xmit message ring
	 */
	for(i = 0; i < Ntrb; i++){
		l.tp[i] = p++;
		lm->tmr[i].size = 0;
		lm->tmr[i].cntflags = 0;
		lm->tmr[i].laddr = LADDR(l.tp[i]);
		lm->tmr[i].flags = HADDR(l.tp[i]);
	}
	lm->tdralow = LADDR(lm->tmr);
	lm->tdrahigh = (LogNtrb<<13)|HADDR(lm->tmr);

	/*
	 *  so that we don't have to indirect through constants
	 */
	l.rap = LANCERAP;
	l.rdp = LANCERDP;

	/*
	 *  point lance to the initialization block
	 */
	*l.rap = 1;
	*l.rdp = LADDR(lm);
	wbflush();
	*l.rap = 2;
	*l.rdp = HADDR(lm);

	/*
	 *  The lance byte swaps the ethernet packet unless we tell it not to
	 */
	wbflush();
	*l.rap = 3;
	*l.rdp = BSWP;

	/*
	 *  initialize lance, turn on interrupts, turn on transmit and rcv.
	 */
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
		lancedir[i].qid = CHDIR|STREAMQID(i, Lchanqid);
		lancedir[i].length = 0;
		lancedir[i].perm = 0600;
	}
	strcpy(lancedir[Ntypes].name, "trace");
	lancedir[Ntypes].qid = Ltraceqid;
	lancedir[Ntypes].length = 0;
	lancedir[Ntypes].perm = 0600;

}

Chan*
lanceattach(char *spec)
{
	Chan *c;

	if(l.kstarted == 0){
		kproc("**lancekproc**", lancekproc, 0);
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
	if(c->qid == CHDIR)
		return devwalk(c, name, lancedir, Ndir, devgen);
	else
		return devwalk(c, name, 0, 0, streamgen);
}

void	 
lancestat(Chan *c, char *dp)
{
	if(c->qid==CHDIR || c->qid==Ltraceqid)
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

	switch(c->qid){
	case CHDIR:
	case Ltraceqid:
		if(omode != OREAD)
			error(0, Eperm);
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
	error(0, Eperm);
}

void	 
lanceclose(Chan *c)
{
	/* real closing happens in lancestclose */
	switch(c->qid){
	case CHDIR:
	case Ltraceqid:
		break;
	default:
		streamclose(c);
		break;
	}
}

static long
lancetraceread(Chan *c, void *a, long n)
{
	char buf[512];
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
	switch(c->qid){
	case CHDIR:
		return devdirread(c, a, n, lancedir, Ndir, devgen);
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
	error(0, Eperm);
}

void	 
lancewstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void	 
lanceerrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void	 
lanceuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

/*
 *  We will:
 *	(1) Clear interrupt cause in the lance
 *	(2) service all current events
 */
void
lanceintr(void)
{
	ushort csr;
	Lancemem *lm;

	lm = LANCEMEM;

	csr = *l.rdp;

	/*
	 *  turn off the interrupt and any error indicators
	 */
	*l.rdp = IDON|INEA|TINT|RINT|BABL|CERR|MISS|MERR;

	/*
	 *  see if an error occurred
	 */
	if(csr & (BABL|MISS|MERR))
		print("lance err %ux\n", csr);

	if(csr & IDON)
		l.inited = 1;

	/*
	 *  look for rcv'd packets, just wakeup the input process
	 */
	if(l.rl!=l.rc && (lm->rmr[l.rl].flags & OWN)==0)
		wakeup(&l.rr);

	/*
	 *  look for xmitt'd packets, wake any process waiting for a
	 *  transmit buffer
	 */
	while(l.tl != l.tc && (lm->tmr[l.tl].flags & OWN) == 0){
		if(lm->tmr[l.tl].flags & ERR)
			print("xmt error %ux %ux\n", lm->tmr[l.tl].flags,
				lm->tmr[l.tl].cntflags);
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
	return l.rl!=l.rc && (LANCEMEM->rmr[l.rl].flags & OWN)==0;
}
void
lancekproc(void *arg)
{
	Block *bp;
	Lancemem *lm;
	Pkt *p;
	Ethertype *e;
	int t;
	Msg *m;
	int len;
	int i, last;

	lm = LANCEMEM;

	for(;;){
		for(; l.rl!=l.rc && (lm->rmr[l.rl].flags & OWN)==0 ; l.rl=RSUCC(l.rl)){
			l.inpackets++;
			m = &(lm->rmr[l.rl]);
			if(m->flags & ERR){
				print("rcv error %ux\n",
					m->flags&(FRAM|OFLO|CRC|BUFF));
				goto stage;
			}
	
			/*
			 *  See if a queue exists for this packet type.
			 */
			p = l.rp[l.rl];
			t = (p->type[0]<<8) | p->type[1];
			len = m->cntflags - 4;
			lancedebq('i', p, len);
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
			if(e != &l.e[Ntypes] && e->q->next->len <= Streamhi){
				/*
				 *  The lock on e makes sure the queue is still there.
				 */
				bp = allocb(len);
				memcpy(bp->rptr, (uchar *)p, len);
				bp->wptr += len;
				bp->flags |= S_DELIM;
				PUTNEXT(e->q, bp);
				qunlock(e);
			}
	
stage:
			/*
			 *  stage the next input buffer
			 */
			m = &(lm->rmr[l.rc]);
			m->size = -sizeof(Pkt);
			m->cntflags = 0;
			m->laddr = LADDR(l.rp[l.rc]);
			m->flags = OWN|HADDR(l.rp[l.rc]);
			l.rc = RSUCC(l.rc);
		}
		sleep(&l.rr, isinput, 0);
	}
}

void
lanceparity(void)
{
	print("lance DRAM parity error lmp=%ux\n", l.lmp);
	MODEREG->promenet &= ~4;
	MODEREG->promenet |= 4;
}
