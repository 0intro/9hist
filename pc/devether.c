/*
 * Western Digital ethernet adapter
 * BUGS:
 *	more than one controller
 *	fix for different controller types
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

typedef struct Ctlr Ctlr;
typedef struct Pktype Pktype;

enum {
	IObase		= 0x360,

	EA		= 0x08,		/* Ethernet Address in ROM */
	ID		= 0x0E,		/* interface type */

	NIC		= 0x10,		/* National DP8390 Chip */
	Cr		= NIC+0x00,	/* Page [01] */

	Clda0		= NIC+0x01,	/* read */
	Pstart		= Clda0,	/* write */
	Clda1		= NIC+0x02,	/* read */
	Pstop		= Clda1,	/* write */
	Bnry		= NIC+0x03,	/* Page 0 */
	Tsr		= NIC+0x04,	/* read */
	Tpsr		= Tsr,		/* write */
	Ncr		= NIC+0x05,
	Isr		= NIC+0x07,
	Rbcr0		= NIC+0x0A,
	Rbcr1		= NIC+0x0B,
	Rsr		= NIC+0x0C,	/* read */
	Rcr		= Rsr,		/* write */
	Cntr0		= NIC+0x0D,	/* read */
	Tcr		= Cntr0,	/* write */
	Cntr1		= NIC+0x0E,	/* read */
	Dcr		= Cntr1,	/* write */
	Cntr2		= NIC+0x0F,	/* read */
	Imr		= Cntr2,	/* write */

	Par0		= NIC+0x01,	/* Page 1 */
	Curr		= NIC+0x07,
	Mar0		= NIC+0x08,
	Mar1		= NIC+0x09,
	Mar2		= NIC+0x0A,
	Mar3		= NIC+0x0B,
	Mar4		= NIC+0x0C,
	Mar5		= NIC+0x0D,
	Mar6		= NIC+0x0E,
	Mar7		= NIC+0x0F,

	RAMbase		= 0xC8000,

	Nctlr		= 1,
	NPktype		= 9,		/* types/interface */
};

/*
 * one per ethernet packet type
 */
struct Pktype {
	QLock;
	int	type;			/* ethernet type */
	int	prom;			/* promiscuous mode */
	Queue	*q;
	int	inuse;
	Ctlr	*ctlr;
};

/*
 *  per ethernet
 */
struct Ctlr {
	QLock;

	Rendez	rr;			/* rendezvous for an input buffer */
	Queue	rq;

	Qlock	xl;
	Rendez	xr;

	int	iobase;			/* I/O base address */

	Pktype	pktype[NPktype];
	uchar	ea[6];
	uchar	ba[6];

	uchar	prom;			/* true if promiscuous mode */
	uchar	kproc;			/* true if kproc started */
	char	name[NAMELEN];		/* name of kproc */
	Network	net;
	Netprot	prot[NPktype];

	int	inpackets;
	int	outpackets;
	int	crcs;			/* input crc errors */
	int	oerrs;			/* output errors */
	int	frames;			/* framing errors */
	int	overflows;		/* packet overflows */
	int	buffs;			/* buffering errors */
};
static Ctlr ctlr[Nctlr];

static void
etheroput(Queue *q, Block *bp)
{
	Ctlr *c;
	int len, n;
	Pktype *t;
	Etherpkt *p;
	Block *nbp;

	c = ((Pktype *)q->ptr)->ctlr;
	if(bp->type == M_CTL){
		t = q->ptr;
		if(streamparse("connect", bp))
			t->type = strtol((char *)bp->rptr, 0, 0);
		else if(streamparse("promiscuous", bp)) {
			t->prom = 1;
			qlock(c);
			c->prom++;
			if(c->prom == 1)
				outb(c->iobase+Rcr, 0x14);	/* PRO|AB */
			qunlock(c);
		}
		freeb(bp);
		return;
	}

	/*
	 * give packet a local address, return upstream if destined for
	 * this machine.
	 */
	if(BLEN(bp) < ETHERHDRSIZE){
		bp = pullup(bp, ETHERHDRSIZE);
		if(bp == 0)
			return;
	}
	p = (Etherpkt *)bp->rptr;
	memmove(p->s, c->ea, sizeof(c->ea));
	if(memcmp(c->ea, p->d, sizeof(c->ea)) == 0){
		len = blen(bp);
		bp = expandb(bp, len >= ETHERMINTU ? len: ETHERMINTU);
		if(bp){
			putq(&c->rq, bp);
			wakeup(&c->rr);
		}
		return;
	}
	if(memcmp(c->ba, p->d, sizeof(c->ba)) == 0){
		len = blen(bp);
		nbp = copyb(bp, len);
		nbp = expandb(nbp, len >= ETHERMINTU ? len: ETHERMINTU);
		if(nbp){
			nbp->wptr = nbp->rptr+len;
			putq(&c->rq, nbp);
			wakeup(&c->rr);
		}
	}

	/*
	 * only one transmitter at a time
	 */
	qlock(&c->xl);
	if(waserror()){
		qunlock(&c->xl);
		nexterror();
	}

	/*
	 *  Wait till we get an output buffer
	 */
	sleep(&c->xr, isobuf, c);
	p = enet.xn;

	/*
	 * copy message into buffer
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
	enet.xn->len = len;

	/*
	 *  give packet a local address
	 */
	memmove(p->s, enet.ea, sizeof(enet.ea));

	/*
	 *  give to Chip
	 */
	splhi();		/* sync with interrupt routine */
	enet.xn->owner = Chip;
	if(enet.xmting == 0)
		ethersend(enet.xn);
	spllo();

	/*
	 *  send
	 */
	enet.xn = XSUCC(enet.xn);
	freeb(bp);
	qunlock(&enet.xl);
	poperror();

	enet.outpackets++;
}

/*
 * open an ether line discipline
 *
 * the lock is to synchronize changing the ethertype with
 * sending packets up the stream on interrupts.
 */
static void
etherstopen(Queue *q, Stream *s)
{
	Ctlr *c = &ctlr[0];
	Pktype *t;

	t = &c->pktype[s->id];
	qlock(t);
	RD(q)->ptr = WR(q)->ptr = t;
	t->type = 0;
	t->q = RD(q);
	t->inuse = 1;
	t->ctlr = c;
	qunlock(t);
}

/*
 *  close ether line discipline
 *
 *  the lock is to synchronize changing the ethertype with
 *  sending packets up the stream on interrupts.
 */
static void
etherstclose(Queue *q)
{
	Pktype *t;

	t = (Pktype *)(q->ptr);
	if(t->prom){
		qlock(t->ctlr);
		t->ctlr->prom--;
		if(t->ctlr->prom == 0)
			/* turn off promiscuous mode here */;
		qunlock(t->ctlr);
	}
	qlock(t);
	t->type = 0;
	t->q = 0;
	t->prom = 0;
	t->inuse = 0;
	netdisown(&t->ctlr->net, t - t->ctlr->pktype);
	qunlock(t);
}

static Qinfo info = {
	nullput,
	etheroput,
	etherstopen,
	etherstclose,
	"ether"
};

static int
clonecon(Chan *chan)
{
	Ctlr *c = &ctlr[0];
	Pktype *t;

	for(t = c->pktype; t < &c->pktype[NPktype]; t++){
		qlock(t);
		if(t->inuse || t->q){
			qunlock(t);
			continue;
		}
		t->inuse = 1;
		netown(&c->net, t - c->pktype, u->p->user, 0);
		qunlock(t);
		return t - c->pktype;
	}
	exhausted("ether channels");
}

static void
statsfill(Chan *chan, char *p, int n)
{
	Ctlr *c = &ctlr[0];
	char buf[256];

	sprint(buf, "in: %d\nout: %d\ncrc errs %d\noverflows: %d\nframe errs %d\nbuff errs: %d\noerrs %d\naddr: %.02x:%.02x:%.02x:%.02x:%.02x:%.02x\n",
		c->inpackets, c->outpackets, c->crcs,
		c->overflows, c->frames, c->buffs, c->oerrs,
		c->ea[0], c->ea[1], c->ea[2], c->ea[3], c->ea[4], c->ea[5]);
	strncpy(p, buf, n);
}

static void
typefill(Chan *c, char *p, int n)
{
	char buf[16];
	Pktype *t;

	t = &ctlr[0].pktype[STREAMID(c->qid.path)];
	sprint(buf, "%d", t->type);
	strncpy(p, buf, n);
}

static void
intr(Ureg *ur)
{
	panic("ether intr\n");
}

/*
 * the following initialisation procedure
 * is mandatory
 * after this, the chip is still offline
 */
static void
init(Ctlr *c)
{
	int i;

	outb(c->iobase+Cr, 0x21);	/* Page0, RD2|STP */
	outb(c->iobase+Dcr, 0x48);	/* FT1|LS */
	outb(c->iobase+Rbcr0, 0);
	outb(c->iobase+Rbcr1, 0);
	outb(c->iobase+Rcr, 0x04);	/* AB */
	outb(c->iobase+Tcr, 0);
	outb(c->iobase+Bnry, 6);
	outb(c->iobase+Pstart, 6);	/* 6*256 */
	outb(c->iobase+Pstop, 32);	/* 8*1024/256 */
	outb(c->iobase+Isr, 0xFF);
	outb(c->iobase+Imr, 0x0F);	/* TXEE|RXEE|PTXE|PRXE */

	outb(c->iobase+Cr, 0x61);	/* Page1, RD2|STP */
	for(i = 0; i < sizeof(c->ea); i++)
		outb(c->iobase+Par0+i, c->ea[i]);
	outb(c->iobase+Curr, 6);	/* 6*256 */
}

void
etherreset(void)
{
	Ctlr *c = &ctlr[0];
	int i;

	c->iobase = IObase;
	init(c);
	setvec(Ethervec, intr);

	for(i = 0; i < sizeof(c->ea); i++)
		c->ea[i] = inb(c->iobase+EA+i);
	memset(c->ba, 0xFF, sizeof(c->ba));

	c->net.name = "ether";
	c->net.nconv = NPktype;
	c->net.devp = &info;
	c->net.protop = 0;
	c->net.listen = 0;
	c->net.clone = clonecon;
	c->net.ninfo = 2;
	c->net.prot = c->prot;
	c->net.info[0].name = "stats";
	c->net.info[0].fill = statsfill;
	c->net.info[1].name = "type";
	c->net.info[1].fill = typefill;
}

static int
isinput(void *arg)
{
	Ctlr *c = arg;

	return 0;
}

static void
etherkproc(void *arg)
{
	Ctlr *c = arg;

	if(waserror()){
		print("someone noted %s\n", c->name);
		c->kproc = 0;
		nexterror();
	}
	c->kproc = 1;
	for(;;){
		sleep(&c->rr, isinput, c);
	}
}

void
etherinit(void)
{
	int ctlrno = 0;

	/*
	 * put the receiver online
	 * and start the kproc
	 */
	outb(c->iobase+Cr, 0x22);	/* Page0, RD2|STA */
	outb(c->iobase+Tpsr, 0);
	if(ctlr[ctlrno].kproc == 0){
		sprint(ctlr[ctlrno].name, "ether%dkproc", ctlrno);
		kproc(ctlr[ctlrno].name, etherkproc, &ctlr[ctlrno]);
	}
}

Chan *
etherattach(char *spec)
{
	return devattach('l', spec);
}

Chan *
etherclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
etherwalk(Chan *c, char *name)
{
	return netwalk(c, name, &ctlr[0].net);
}

void
etherstat(Chan *c, char *dp)
{
	netstat(c, dp, &ctlr[0].net);
}

Chan *
etheropen(Chan *c, int omode)
{
	return netopen(c, omode, &ctlr[0].net);
}

void
ethercreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
etherremove(Chan *c)
{
	error(Eperm);
}

void
etherwstat(Chan *c, char *dp)
{
	netwstat(c, dp, &ctlr[0].net);
}

void
etherclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long
etherread(Chan *c, void *a, long n, ulong offset)
{
	return netread(c, a, n, offset, &ctlr[0].net);
}

long
etherwrite(Chan *c, char *a, long n, ulong offset)
{
	return streamwrite(c, a, n, 0);
}
