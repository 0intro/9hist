/*
 * Western Digital ethernet adapter
 * BUGS:
 *	no more than one controller
 * TODO:
 *	fix for different controller types
 *	output
 *	deal with stalling and restarting output on input overflow
 *	fix magic ring constants
 *	rewrite per SMC doc
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
typedef struct Ring Ring;

enum {
	IObase		= 0x360,
	RAMbase		= 0xC8000,
	RAMsize		= 8*1024,
	BUFsize		= 256,

	Nctlr		= 1,
	NPktype		= 9,		/* types/interface */
};

#define NEXT(x, l)	((((x)+1)%(l)) == 0 ? 6: (((x)+1)%(l)))
#define PREV(x, l)	(((x)-1) == 5 ? (l-1): ((x)-1))

/*
 * register offsets from IObase
 */
enum {
	EA		= 0x08,		/* Ethernet Address in ROM */
	ID		= 0x0E,		/* interface type */

	NIC		= 0x10,		/* National DP8390 Chip */
	Cr		= NIC+0x00,	/* Page [01] */

	Pstart		= NIC+0x01,	/* write */
	Pstop		= NIC+0x02,	/* write */
	Bnry		= NIC+0x03,
	Tsr		= NIC+0x04,	/* read */
	Tpsr		= Tsr,		/* write */
	Tbcr0		= NIC+0x05,	/* write */
	Tbcr1		= NIC+0x06,	/* write */
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
};

/*
 * some register bits
 */
enum {
	Prx		= 0x01,		/* Isr:	packet received */
	Ptx		= 0x02,		/*	packet transmitted */
	Rxe		= 0x04,		/*	receive error */
	Txe		= 0x08,		/*	transmit error */
	Ovw		= 0x10,		/*	overwrite warning */
	Rst		= 0x80,		/*	reset status */
};

struct Ring {
	uchar	status;
	uchar	next;
	uchar	len0;
	uchar	len1;
	uchar	data[BUFsize-4];
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

	Ring	*ring;
	Rendez	rr;			/* rendezvous for an input buffer */
	Queue	rq;
	uchar	bnry;
	uchar	curr;
	uchar	ovw;

	Etherpkt *xpkt;
	QLock	xl;
	Rendez	xr;
	uchar	xbusy;

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

static int
isxfree(void *arg)
{
	Ctlr *cp = arg;

	return cp->xbusy == 0 && cp->ovw == 0;
}

static void
etheroput(Queue *q, Block *bp)
{
	Ctlr *cp;
	int len, n;
	Pktype *pp;
	Etherpkt *p;
	Block *nbp;

	cp = ((Pktype *)q->ptr)->ctlr;
	if(bp->type == M_CTL){
		pp = q->ptr;
		if(streamparse("connect", bp))
			pp->type = strtol((char *)bp->rptr, 0, 0);
		else if(streamparse("promiscuous", bp)) {
			pp->prom = 1;
			qlock(cp);
			cp->prom++;
			if(cp->prom == 1)
				outb(cp->iobase+Rcr, 0x14);	/* PRO|AB */
			qunlock(cp);
		}
		freeb(bp);
		return;
	}

	/*
	 * give packet a local address, return upstream if destined for
	 * this machine.
	 */
	if(BLEN(bp) < ETHERHDRSIZE && (bp = pullup(bp, ETHERHDRSIZE)) == 0)
		return;
	p = (Etherpkt *)bp->rptr;
	memmove(p->s, cp->ea, sizeof(cp->ea));
	if(memcmp(cp->ea, p->d, sizeof(cp->ea)) == 0){
		len = blen(bp);
		if (bp = expandb(bp, len >= ETHERMINTU ? len: ETHERMINTU)){
			putq(&cp->rq, bp);
			wakeup(&cp->rr);
		}
		return;
	}
	if(memcmp(cp->ba, p->d, sizeof(cp->ba)) == 0){
		len = blen(bp);
		nbp = copyb(bp, len);
		if(nbp = expandb(nbp, len >= ETHERMINTU ? len: ETHERMINTU)){
			nbp->wptr = nbp->rptr+len;
			putq(&cp->rq, nbp);
			wakeup(&cp->rr);
		}
	}

	/*
	 * only one transmitter at a time
	 */
	qlock(&cp->xl);
	if(waserror()){
		qunlock(&cp->xl);
		nexterror();
	}

	/*
	 * Wait till we get an output buffer
	 */
	sleep(&cp->xr, isxfree, cp);
	p = cp->xpkt;

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
	 * pad the packet (zero the pad)
	 */
	if(len < ETHERMINTU){
		memset(((char*)p)+len, 0, ETHERMINTU-len);
		len = ETHERMINTU;
	}

	/*
	 * give packet a local address
	 */
	memmove(p->s, cp->ea, sizeof(cp->ea));

	/*
	 * start the transmission
	 */
	outb(cp->iobase+Tbcr0, len & 0xFF);
	outb(cp->iobase+Tbcr1, (len>>8) & 0xFF);
	outb(cp->iobase+Cr, 0x26);			/* Page0|TXP|STA */
	cp->xbusy = 1;

	freeb(bp);
	qunlock(&cp->xl);
	poperror();
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
	Ctlr *cp = &ctlr[0];
	Pktype *pp;

	pp = &cp->pktype[s->id];
	qlock(pp);
	RD(q)->ptr = WR(q)->ptr = pp;
	pp->type = 0;
	pp->q = RD(q);
	pp->inuse = 1;
	pp->ctlr = cp;
	qunlock(pp);
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
	Pktype *pp;

	pp = (Pktype *)(q->ptr);
	if(pp->prom){
		qlock(pp->ctlr);
		pp->ctlr->prom--;
		if(pp->ctlr->prom == 0)
			outb(pp->ctlr->iobase+Rcr, 0x04);/* AB */
		qunlock(pp->ctlr);
	}
	qlock(pp);
	pp->type = 0;
	pp->q = 0;
	pp->prom = 0;
	pp->inuse = 0;
	netdisown(&pp->ctlr->net, pp - pp->ctlr->pktype);
	qunlock(pp);
}

static Qinfo info = {
	nullput,
	etheroput,
	etherstopen,
	etherstclose,
	"ether"
};

static int
clonecon(Chan *c)
{
	Ctlr *cp = &ctlr[0];
	Pktype *pp;

	for(pp = cp->pktype; pp < &cp->pktype[NPktype]; pp++){
		qlock(pp);
		if(pp->inuse || pp->q){
			qunlock(pp);
			continue;
		}
		pp->inuse = 1;
		netown(&cp->net, pp - cp->pktype, u->p->user, 0);
		qunlock(pp);
		return pp - cp->pktype;
	}
	exhausted("ether channels");
}

static void
statsfill(Chan *c, char *p, int n)
{
	Ctlr *cp = &ctlr[0];
	char buf[256];

	sprint(buf, "in: %d\nout: %d\ncrc errs %d\noverflows: %d\nframe errs %d\nbuff errs: %d\noerrs %d\naddr: %.02x:%.02x:%.02x:%.02x:%.02x:%.02x\n",
		cp->inpackets, cp->outpackets, cp->crcs,
		cp->overflows, cp->frames, cp->buffs, cp->oerrs,
		cp->ea[0], cp->ea[1], cp->ea[2], cp->ea[3], cp->ea[4], cp->ea[5]);
	strncpy(p, buf, n);
}

static void
typefill(Chan *c, char *p, int n)
{
	char buf[16];
	Pktype *pp;

	pp = &ctlr[0].pktype[STREAMID(c->qid.path)];
	sprint(buf, "%d", pp->type);
	strncpy(p, buf, n);
}

static void
intr(Ureg *ur)
{
	Ctlr *cp = &ctlr[0];
	uchar isr, curr;

	while(isr = inb(cp->iobase+Isr)){
		outb(cp->iobase+Isr, isr);
		if(isr & Txe)
			cp->oerrs++;
		if(isr & Rxe){
			cp->frames += inb(cp->iobase+Cntr0);
			cp->crcs += inb(cp->iobase+Cntr1);
			cp->buffs += inb(cp->iobase+Cntr2);
		}
		if(isr & Ptx)
			cp->outpackets++;
		if(isr & (Txe|Ptx)){
			cp->xbusy = 0;
			wakeup(&cp->xr);
		}
		/*
		 * the receive ring is full.
		 * put the NIC into loopback mode to give
		 * kproc a chance to process some packets.
		 */
		if(isr & Ovw){
			outb(cp->iobase+Cr, 0x21);	/* Page0, RD2|STP */
			outb(cp->iobase+Rbcr0, 0);
			outb(cp->iobase+Rbcr1, 0);
			while((inb(cp->iobase+Isr) & Rst) == 0)
				delay(1);
			outb(cp->iobase+Tcr, 0x20);	/* LB0 */
			outb(cp->iobase+Cr, 0x22);	/* Page0, RD2|STA */
			cp->ovw = 1;
			cp->overflows++;
		}
		/*
		 * we have received packets.
		 * this is the only place, other than the init code,
		 * where we set the controller to Page1.
		 * we must be sure to reset it back to Page0 in case
		 * we interrupted some other part of this driver.
		 */
		if(isr & (Ovw|Prx)){
			outb(cp->iobase+Cr, 0x62);	/* Page1, RD2|STA */
			cp->curr = inb(cp->iobase+Curr);
			outb(cp->iobase+Cr, 0x22);	/* Page0, RD2|STA */
			wakeup(&cp->rr);
		}
	}
}

/*
 * the following initialisation procedure
 * is mandatory.
 * we leave the chip idling on internal loopback
 * and pointing to Page0.
 */
static void
init(Ctlr *cp)
{
	int i;

	outb(cp->iobase+Cr, 0x21);		/* Page0, RD2|STP */
	outb(cp->iobase+Dcr, 0x48);		/* FT1|LS */
	outb(cp->iobase+Rbcr0, 0);
	outb(cp->iobase+Rbcr1, 0);
	outb(cp->iobase+Rcr, 0x04);		/* AB */
	outb(cp->iobase+Tcr, 0x20);		/* LB0 */
	cp->bnry = 6;
	outb(cp->iobase+Bnry, cp->bnry);
	cp->ring = (Ring*)(KZERO|RAMbase);
	outb(cp->iobase+Pstart, 6);		/* 6*256 */
	outb(cp->iobase+Pstop, 32);		/* 8*1024/256 */
	outb(cp->iobase+Isr, 0xFF);
	outb(cp->iobase+Imr, 0x1F);		/* OVWE|TXEE|RXEE|PTXE|PRXE */

	outb(cp->iobase+Cr, 0x61);		/* Page1, RD2|STP */
	for(i = 0; i < sizeof(cp->ea); i++)
		outb(cp->iobase+Par0+i, cp->ea[i]);
	cp->curr = cp->bnry+1;
	outb(cp->iobase+Curr, cp->curr);

	outb(cp->iobase+Cr, 0x22);		/* Page0, RD2|STA */
	outb(cp->iobase+Tpsr, 0);
	cp->xpkt = (Etherpkt*)(KZERO|RAMbase);
}

void
etherreset(void)
{
	Ctlr *cp = &ctlr[0];
	int i;

	cp->iobase = IObase;
	for(i = 0; i < sizeof(cp->ea); i++)
		cp->ea[i] = inb(cp->iobase+EA+i);
	init(cp);
	setvec(Ethervec, intr);
	memset(cp->ba, 0xFF, sizeof(cp->ba));

	cp->net.name = "ether";
	cp->net.nconv = NPktype;
	cp->net.devp = &info;
	cp->net.protop = 0;
	cp->net.listen = 0;
	cp->net.clone = clonecon;
	cp->net.ninfo = 2;
	cp->net.prot = cp->prot;
	cp->net.info[0].name = "stats";
	cp->net.info[0].fill = statsfill;
	cp->net.info[1].name = "type";
	cp->net.info[1].fill = typefill;
}

static void
etherup(Ctlr *cp, Etherpkt *p, int len)
{
	Block *bp;
	Pktype *pp;
	int t;

	t = (p->type[0]<<8) | p->type[1];
	for(pp = &cp->pktype[0]; pp < &cp->pktype[NPktype]; pp++){
		/*
		 *  check before locking just to save a lock
		 */
		if(pp->q == 0 || (t != pp->type && pp->type != -1))
			continue;

		/*
		 *  only a trace channel gets packets destined for other machines
		 */
		if(pp->type != -1 && p->d[0] != 0xFF && memcmp(p->d, cp->ea, sizeof(p->d)))
			continue;

		/*
		 *  check after locking to make sure things didn't
		 *  change under foot
		 */
		if(canqlock(pp) == 0)
			continue;
		if(pp->q == 0 || pp->q->next->len > Streamhi || (t != pp->type && pp->type != -1)){
			qunlock(pp);
			continue;
		}
		if(waserror() == 0){
			bp = allocb(len);
			memmove(bp->rptr, (uchar *)p, len);
			bp->wptr += len;
			bp->flags |= S_DELIM;
			PUTNEXT(pp->q, bp);
		}
		poperror();
		qunlock(pp);
	}
}

static void
printpkt(uchar bnry, ushort len, Etherpkt *p)
{
	int i;

	print("%.2ux: %.4d d(%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux)s(%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux)t(%.2ux %.2ux)\n",
		bnry, len,
		p->d[0], p->d[1], p->d[2], p->d[3], p->d[4], p->d[5],
		p->s[0], p->s[1], p->s[2], p->s[3], p->s[4], p->s[5],
		p->type[0], p->type[1]);
}

static int
isinput(void *arg)
{
	Ctlr *cp = arg;

	return NEXT(cp->bnry, 32) != cp->curr;
}

static void
etherkproc(void *arg)
{
	Ctlr *cp = arg;
	Block *bp;
	uchar bnry, curr;
	Ring *rp;

	if(waserror()){
		print("%s noted\n", cp->name);
		init(cp);
		cp->kproc = 0;
		nexterror();
	}
	cp->kproc = 1;
	for(;;){
		sleep(&cp->rr, isinput, cp);
		/*
		 * process any internal loopback packets
		 */
		while(bp = getq(&cp->rq)){
			cp->inpackets++;
			etherup(cp, (Etherpkt*)bp->rptr, BLEN(bp));
			freeb(bp);
		}

		/*
		 * process any received packets
		 */
		bnry = NEXT(cp->bnry, 32);
		while(bnry != cp->curr){
			rp = &cp->ring[bnry];
			cp->inpackets++;
			etherup(cp, (Etherpkt*)rp->data, ((rp->len1<<8)+rp->len0)-4);
			bnry = rp->next;
			cp->bnry = PREV(bnry, 32);
			outb(cp->iobase+Bnry, cp->bnry);
		}

		/*
		 * if we idled input because of overflow,
		 * restart
		 */
		if(cp->ovw){
			cp->ovw = 0;
			outb(cp->iobase+Tcr, 0);
			wakeup(&cp->xr);
		}
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
	outb(ctlr[ctlrno].iobase+Tcr, 0);
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
