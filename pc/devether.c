#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

/*
 * Half-arsed attempt at a general top-level
 * ethernet driver. Needs work:
 *	handle multiple controllers
 *	much tidying
 *	set ethernet address
 */
extern EtherHw ether509;
extern EtherHw ether80x3;

static EtherHw *etherhw[] = {
	&ether509,
	&ether80x3,
	0
};

enum {
	Nctlr		= 1,
};

static struct EtherCtlr ctlr[Nctlr];

#define NEXT(x, l)	(((x)+1)%(l))
#define OFFSETOF(t, m)	((unsigned)&(((t*)0)->m))
#define	HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))

Chan*
etherattach(char *spec)
{
	if(ctlr[0].present == 0)
		error(Enodev);
	return devattach('l', spec);
}

Chan*
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

Chan*
etheropen(Chan *c, int omode)
{
	return netopen(c, omode, &ctlr[0].net);
}

void
ethercreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
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
	USED(offset);
	return streamwrite(c, a, n, 0);
}

void
etherremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
etherwstat(Chan *c, char *dp)
{
	netwstat(c, dp, &ctlr[0].net);
}

static int
isobuf(void *arg)
{
	EtherCtlr *cp = arg;

	return cp->tb[cp->th].owner == Host;
}

static void
etheroput(Queue *q, Block *bp)
{
	EtherCtlr *cp;
	EtherType *tp;
	Etherpkt *p;
	EtherBuf *tb;
	int len, n;
	Block *nbp;

	if(bp->type == M_CTL){
		tp = q->ptr;
		if(streamparse("connect", bp))
			tp->type = strtol((char*)bp->rptr, 0, 0);
		else if(streamparse("promiscuous", bp)) {
			tp->prom = 1;
			(*tp->ctlr->hw->mode)(tp->ctlr, 1);
		}
		freeb(bp);
		return;
	}

	cp = ((EtherType*)q->ptr)->ctlr;

	/*
	 * give packet a local address, return upstream if destined for
	 * this machine.
	 */
	if(BLEN(bp) < ETHERHDRSIZE && (bp = pullup(bp, ETHERHDRSIZE)) == 0)
		return;
	p = (Etherpkt*)bp->rptr;
	memmove(p->s, cp->ea, sizeof(cp->ea));
	if(memcmp(cp->ea, p->d, sizeof(cp->ea)) == 0){
		len = blen(bp);
		if (bp = expandb(bp, len >= ETHERMINTU ? len: ETHERMINTU)){
			putq(&cp->lbq, bp);
			wakeup(&cp->rr);
		}
		return;
	}
	if(memcmp(cp->ba, p->d, sizeof(cp->ba)) == 0){
		len = blen(bp);
		nbp = copyb(bp, len);
		if(nbp = expandb(nbp, len >= ETHERMINTU ? len: ETHERMINTU)){
			nbp->wptr = nbp->rptr+len;
			putq(&cp->lbq, nbp);
			wakeup(&cp->rr);
		}
	}

	/*
	 * only one transmitter at a time
	 */
	qlock(&cp->tlock);
	if(waserror()){
		freeb(bp);
		qunlock(&cp->tlock);
		nexterror();
	}

	/*
	 * wait till we get an output buffer.
	 * should try to restart.
	 */
	sleep(&cp->tr, isobuf, cp);

	tb = &cp->tb[cp->th];

	/*
	 * copy message into buffer
	 */
	len = 0;
	for(nbp = bp; nbp; nbp = nbp->next){
		if(sizeof(Etherpkt) - len >= (n = BLEN(nbp))){
			memmove(tb->pkt+len, nbp->rptr, n);
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
		memset(tb->pkt+len, 0, ETHERMINTU-len);
		len = ETHERMINTU;
	}

	/*
	 * set up the transmit buffer and 
	 * start the transmission
	 */
	cp->outpackets++;
	tb->len = len;
	tb->owner = Interface;
	cp->th = NEXT(cp->th, cp->ntb);
	(*cp->hw->transmit)(cp);

	freeb(bp);
	qunlock(&cp->tlock);
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
	EtherCtlr *cp = &ctlr[0];
	EtherType *tp;

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
	EtherType *tp;

	tp = (EtherType*)(q->ptr);
	if(tp->prom)
		(*tp->ctlr->hw->mode)(tp->ctlr, 0);
	qlock(tp);
	tp->type = 0;
	tp->q = 0;
	tp->prom = 0;
	tp->inuse = 0;
	netdisown(tp);
	tp->ctlr = 0;
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
	EtherCtlr *cp = &ctlr[0];
	EtherType *tp;

	USED(c);
	for(tp = cp->type; tp < &cp->type[NType]; tp++){
		qlock(tp);
		if(tp->inuse || tp->q){
			qunlock(tp);
			continue;
		}
		tp->inuse = 1;
		netown(tp, u->p->user, 0);
		qunlock(tp);
		return tp - cp->type;
	}
	exhausted("ether channels");
	return 0;
}

static void
statsfill(Chan *c, char *p, int n)
{
	EtherCtlr *cp = &ctlr[0];
	char buf[256];

	USED(c);
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
	EtherType *tp;

	tp = &ctlr[0].type[STREAMID(c->qid.path)];
	sprint(buf, "%d", tp->type);
	strncpy(p, buf, n);
}

static void
etherup(EtherCtlr *cp, void *data, int len)
{
	Etherpkt *p;
	int t;
	EtherType *tp;
	Block *bp;

	p = data;
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
			bp = allocb(len);
			memmove(bp->rptr, p, len);
			bp->wptr += len;
			bp->flags |= S_DELIM;
			PUTNEXT(tp->q, bp);
		}
		poperror();
		qunlock(tp);
	}
}

static int
isinput(void *arg)
{
	EtherCtlr *cp = arg;

	return cp->lbq.first || cp->rb[cp->ri].owner == Host;
}

static void
etherkproc(void *arg)
{
	EtherCtlr *cp = arg;
	EtherBuf *rb;
	Block *bp;

	if(waserror()){
		print("%s noted\n", cp->name);
		if(cp->hw->init)
			(*cp->hw->init)(cp);
		cp->kproc = 0;
		nexterror();
	}
	cp->kproc = 1;
	for(;;){
		tsleep(&cp->rr, isinput, cp, 500);
		if(cp->hw->tweak)
			(*cp->hw->tweak)(cp);

		/*
		 * process any internal loopback packets
		 */
		while(bp = getq(&cp->lbq)){
			cp->inpackets++;
			etherup(cp, bp->rptr, BLEN(bp));
			freeb(bp);
		}

		/*
		 * process any received packets
		 */
		while(cp->rb[cp->rh].owner == Host){
			cp->inpackets++;
			rb = &cp->rb[cp->rh];
			etherup(cp, rb->pkt, rb->len);
			rb->owner = Interface;
			cp->rh = NEXT(cp->rh, cp->nrb);
		}
	}
}

static void
etherintr(Ureg *ur)
{
	EtherCtlr *cp = &ctlr[0];

	USED(ur);
	(*cp->hw->intr)(cp);
}

void
etherreset(void)
{
	EtherCtlr *cp;
	EtherHw **hw;
	int i;

	cp = &ctlr[0];
	for(hw = etherhw; *hw; hw++){
		cp->hw = *hw;
		if((*cp->hw->reset)(cp) == 0){
			cp->present = 1;
			setvec(Int0vec + cp->hw->irq, etherintr);
			break;
		}
	}
	if(cp->present == 0)
		return;

	memset(cp->ba, 0xFF, sizeof(cp->ba));

	cp->net.name = "ether";
	cp->net.nconv = NType;
	cp->net.devp = &info;
	cp->net.protop = 0;
	cp->net.listen = 0;
	cp->net.clone = clonecon;
	cp->net.ninfo = 2;
	cp->net.info[0].name = "stats";
	cp->net.info[0].fill = statsfill;
	cp->net.info[1].name = "type";
	cp->net.info[1].fill = typefill;
	for(i = 0; i < NType; i++)
		netadd(&cp->net, &cp->type[i], i);
}

void
etherinit(void)
{
	int ctlrno = 0;
	EtherCtlr *cp = &ctlr[ctlrno];
	int i;

	if(cp->present == 0)
		return;

	cp->rh = 0;
	cp->ri = 0;
	for(i = 0; i < cp->nrb; i++)
		cp->rb[i].owner = Interface;

	cp->th = 0;
	cp->ti = 0;
	for(i = 0; i < cp->ntb; i++)
		cp->tb[i].owner = Host;

	/*
	 * put the receiver online
	 * and start the kproc
	 */	
	(*cp->hw->online)(cp, 1);
	if(cp->kproc == 0){
		sprint(cp->name, "ether%dkproc", ctlrno);
		kproc(cp->name, etherkproc, cp);
	}
}
