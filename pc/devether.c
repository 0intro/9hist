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

static int debug;

typedef struct Ctlr Ctlr;
typedef struct Type Type;
typedef struct Ring Ring;

enum {
	IObase		= 0x360,
	RAMbase		= 0xC8000,
	RAMsize		= 8*1024,
	BUFsize		= 256,

	RINGbase	= 6,		/* gak */
	RINGsize	= 32,		/* gak */

	Nctlr		= 1,
	NType		= 9,		/* types/interface */
};

#define NEXT(x, l)	((((x)+1)%(l)) == 0 ? RINGbase: (((x)+1)%(l)))
#define PREV(x, l)	(((x)-1) < RINGbase ? (l-1): ((x)-1))

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
struct Type {
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

	Etherpkt *xpkt;
	QLock	xl;
	Rendez	xr;
	uchar	xbusy;

	int	iobase;			/* I/O base address */

	Type	type[NType];
	uchar	ea[6];
	uchar	ba[6];

	uchar	prom;			/* true if promiscuous mode */
	uchar	kproc;			/* true if kproc started */
	char	name[NAMELEN];		/* name of kproc */
	Network	net;
	Netprot	prot[NType];

	int	inpackets;
	int	outpackets;
	int	crcs;			/* input crc errors */
	int	oerrs;			/* output errors */
	int	frames;			/* framing errors */
	int	overflows;		/* packet overflows */
	int	buffs;			/* buffering errors */
};
static Ctlr ctlr[Nctlr];

static Etherpkt txpkt;

static void
xmemmove(void *to, void *from, long len)
{
	ushort *t, *f;
	int s;
	Ctlr *cp = &ctlr[0];
	uchar reg;

	t = to;
	f = from;
	len = (len+1)/2;
	s = splhi();
	reg = inb(cp->iobase+0x05);
	outb(cp->iobase+Imr, 0);
	outb(cp->iobase+0x05, 0x80|reg);
	while(len--)
		*t++ = *f++;
	outb(cp->iobase+0x05, reg);
	outb(cp->iobase+Imr, 0x1F);
	splx(s);
}

static int
isxfree(void *arg)
{
	Ctlr *cp = arg;

	return cp->xbusy == 0;
}

static void
etheroput(Queue *q, Block *bp)
{
	Ctlr *cp;
	int len, n;
	Type *tp;
	Etherpkt *p;
	Block *nbp;

	cp = ((Type *)q->ptr)->ctlr;
	if(bp->type == M_CTL){
		tp = q->ptr;
		if(streamparse("connect", bp))
			tp->type = strtol((char *)bp->rptr, 0, 0);
		else if(streamparse("promiscuous", bp)) {
			tp->prom = 1;
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
		freeb(bp);
		qunlock(&cp->xl);
		nexterror();
	}

	/*
	 * Wait till we get an output buffer
	 */
	tsleep(&cp->xr, isxfree, cp, 1000);
	if(isxfree(cp) == 0)
		print("Tx wedged\n");
	p = &txpkt;

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
	xmemmove(cp->xpkt, p, len);

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
	Type *tp;

	tp = &cp->type[s->id];
	qlock(tp);
	RD(q)->ptr = WR(q)->ptr = tp;
	tp->type = 0;
	tp->q = RD(q);
	tp->inuse = 1;
	tp->ctlr = cp;
	qunlock(tp);
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
	Type *tp;

	tp = (Type *)(q->ptr);
	if(tp->prom){
		qlock(tp->ctlr);
		tp->ctlr->prom--;
		if(tp->ctlr->prom == 0)
			outb(tp->ctlr->iobase+Rcr, 0x04);/* AB */
		qunlock(tp->ctlr);
	}
	qlock(tp);
	tp->type = 0;
	tp->q = 0;
	tp->prom = 0;
	tp->inuse = 0;
	netdisown(&tp->ctlr->net, tp - tp->ctlr->type);
	qunlock(tp);
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
	Type *tp;

	for(tp = cp->type; tp < &cp->type[NType]; tp++){
		qlock(tp);
		if(tp->inuse || tp->q){
			qunlock(tp);
			continue;
		}
		tp->inuse = 1;
		netown(&cp->net, tp - cp->type, u->p->user, 0);
		qunlock(tp);
		return tp - cp->type;
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
	Type *tp;

	tp = &ctlr[0].type[STREAMID(c->qid.path)];
	sprint(buf, "%d", tp->type);
	strncpy(p, buf, n);
}

static void
intr(Ureg *ur)
{
	Ctlr *cp = &ctlr[0];
	uchar isr, bnry, curr;

	while(isr = inb(cp->iobase+Isr)){
		outb(cp->iobase+Isr, isr);
		if(isr & Txe)
			cp->oerrs++;
		if(isr & Rxe){
			cp->frames += inb(cp->iobase+Cntr0);
			cp->crcs += inb(cp->iobase+Cntr1);
			cp->overflows += inb(cp->iobase+Cntr2);
		}
		if(isr & Ptx)
			cp->outpackets++;
		if(isr & (Txe|Ptx)){
			cp->xbusy = 0;
			wakeup(&cp->xr);
		}
		if(isr & Ovw){
			bnry = inb(cp->iobase+Bnry);
			outb(cp->iobase+bnry, bnry);
			cp->buffs++;
		}
		/*
		 * we have received packets.
		 * this is the only place, other than the init code,
		 * where we set the controller to Page1.
		 * we must be sure to reset it back to Page0 in case
		 * we interrupted some other part of this driver.
		 */
		if(isr & (Rxe|Prx)){
			outb(cp->iobase+Cr, 0x62);	/* Page1, RD2|STA */
			cp->curr = inb(cp->iobase+Curr);
			outb(cp->iobase+Cr, 0x22);	/* Page0, RD2|STA */
if(debug)
    print("I%d/%d/%d|", isr, cp->curr, cp->bnry);
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
	cp->bnry = RINGbase;
	outb(cp->iobase+Bnry, cp->bnry);
	cp->ring = (Ring*)(KZERO|RAMbase);
	outb(cp->iobase+Pstart, RINGbase);
	outb(cp->iobase+Pstop, RINGsize);
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
	uchar reg;

	cp->iobase = IObase;
	reg = 0x40|inb(cp->iobase);
	outb(cp->iobase, reg);
	reg = 0x40|inb(cp->iobase+0x05);
	outb(cp->iobase+0x05, reg);
for(i = 0; i < 0x10; i++)
    print("#%2.2ux ", inb(cp->iobase+i));
print("\n");
	for(i = 0; i < sizeof(cp->ea); i++)
		cp->ea[i] = inb(cp->iobase+EA+i);
	init(cp);
	setvec(Ethervec, intr);
	memset(cp->ba, 0xFF, sizeof(cp->ba));

	cp->net.name = "ether";
	cp->net.nconv = NType;
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
etherup(Ctlr *cp, uchar *d0, int len0, uchar *d1, int len1)
{
	Etherpkt *p;
	Block *bp;
	Type *tp;
	int t;

	p = (Etherpkt*)d0;
	t = (p->type[0]<<8)|p->type[1];
	for(tp = &cp->type[0]; tp < &cp->type[NType]; tp++){
		/*
		 *  check before locking just to save a lock
		 */
		if(tp->q == 0 || (t != tp->type && tp->type != -1))
			continue;

		/*
		 *  only a trace channel gets packets destined for other machines
		 */
		if(tp->type != -1 && p->d[0] != 0xFF && memcmp(p->d, cp->ea, sizeof(p->d)))
			continue;

		/*
		 *  check after locking to make sure things didn't
		 *  change under foot
		 */
		if(canqlock(tp) == 0)
			continue;
		if(tp->q == 0 || tp->q->next->len > Streamhi || (t != tp->type && tp->type != -1)){
			qunlock(tp);
			continue;
		}
		if(waserror() == 0){
			bp = allocb(len0+len1);
			xmemmove(bp->rptr, d0, len0);
			if(len1)
				xmemmove(bp->rptr+len0, d1, len1);
			bp->wptr += len0+len1;
			bp->flags |= S_DELIM;
			PUTNEXT(tp->q, bp);
		}
		poperror();
		qunlock(tp);
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

	return NEXT(cp->bnry, RINGsize) != cp->curr;
}

static void
etherkproc(void *arg)
{
	Ctlr *cp = arg;
	Block *bp;
	uchar bnry, curr;
	Ring *rp;
	int len0, len1;

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
			etherup(cp, bp->rptr, BLEN(bp), 0, 0);
			freeb(bp);
		}

		/*
		 * process any received packets
		 */
		bnry = NEXT(cp->bnry, RINGsize);
		while(bnry != cp->curr){
			rp = &cp->ring[bnry];
			cp->inpackets++;
			len0 = ((rp->len1<<8)+rp->len0)-4;
			len1 = 0;

			if(rp->data+len0 >= (uchar*)&cp->ring[RINGsize]){
				len1 = rp->data+len0 - (uchar*)&cp->ring[RINGsize];
				len0 = (uchar*)&cp->ring[RINGsize] - rp->data;
			}

			etherup(cp, rp->data, len0, (uchar*)&cp->ring[RINGbase], len1);



if(debug)
    print("K%d/%d/%d|", bnry, rp->next, PREV(rp->next, RINGsize));
			bnry = rp->next;
			cp->bnry = PREV(bnry, RINGsize);
			outb(cp->iobase+Bnry, cp->bnry);
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

void
consdebug(void)
{
	Ctlr *cp = &ctlr[0];
	uchar bnry, curr, isr;
	int s;

	s = splhi();
debug++;
	bnry = inb(cp->iobase+Bnry);
	outb(cp->iobase+Cr, 0x62);			/* Page1, RD2|STA */
	curr = inb(cp->iobase+Curr);
	outb(cp->iobase+Cr, 0x22);			/* Page0, RD2|STA */
	isr = inb(cp->iobase+Isr);
	print("b%d c%d x%d B%d C%d I%ux",
	    cp->bnry, cp->curr, cp->xbusy, bnry, curr, isr);
	print("\t%d %d %d %d %d %d %d\n", cp->inpackets, cp->outpackets,
	    cp->crcs, cp->oerrs, cp->frames, cp->overflows, cp->buffs);
	splx(s);
}
