#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"netif.h"

typedef struct Link	Link;
typedef struct Loop	Loop;

struct Link
{
	Lock;

	int	ref;

	int	nodrop;		/* disable dropping on iq overflow */
	int	soverflows;	/* packets dropped because iq overflowed */
	int	drops;		/* packets deliberately dropped */

	long	delay0;		/* fastticks of delay in the link */
	long	delayn;		/* fastticks of delay per byte */

	Block	*tq;		/* transmission queue */
	Block	*tqtail;
	vlong	tout;		/* time the last packet in tq is really out */
	vlong	tin;		/* time the head packet in tq enters the remote side  */

	Queue	*oq;		/* output queue from other side & packets in the link */
	Queue	*iq;
};

struct Loop
{
	QLock;
	int	ref;
	int	minmtu;		/* smallest block transmittable */
	Loop	*next;
	ulong	path;
	long	limit;		/* queue buffering limit */
	Link	link[2];
};

static struct
{
	Lock;
	ulong	path;
} loopbackalloc;

enum
{
	Qdir,
	Qctl,
	Qstatus,
	Qstats,
	Qdata0,
	Qdata1,

	TMSIZE		= 8,

	NLOOPBACKS	= 1,
	LOOPBACKSIZE	= 32*1024,		/*ZZZ change to settable; size of queues */
};

Dirtab loopbackdir[] =
{
	"ctl",		{Qctl},		0,			0222,
	"status",	{Qstatus},	0,			0222,
	"stats",	{Qstats},	0,			0444,
	"data",		{Qdata0},	0,			0666,
	"data1",	{Qdata1},	0,			0666,
};

static Loop	loopbacks[NLOOPBACKS];

static void	looper(Loop *lb);
static long	loopoput(Loop *lb, Link *link, Block *bp);
static void	ptime(uchar *p, vlong t);
static vlong	gtime(uchar *p);
static void	closelink(Link *link, int dofree);
static vlong	pushlink(Link *link, vlong now);
static void	freelb(Loop *lb);

static void
loopbackinit(void)
{
	int i;

	for(i = 0; i < NLOOPBACKS; i++)
		loopbacks[i].path = i;
}

static Chan*
loopbackattach(char *spec)
{
	Loop *lb;
	Queue *q;
	Chan *c;
	int chan;

	c = devattach('X', spec);
	lb = &loopbacks[0];

	qlock(lb);
	if(waserror()){
		qunlock(lb);
		nexterror();
	}

	lb->ref++;
	if(lb->ref == 1){
		lb->limit = LOOPBACKSIZE;
		for(chan = 0; chan < 2; chan++){
			q = qopen(lb->limit, 0, 0, 0);
			lb->link[chan].iq = q;
			if(q == nil){
				freelb(lb);
				exhausted("memory");
			}
			q = qopen(lb->limit, 0, 0, 0);
			lb->link[chan].oq = q;
			if(q == nil){
				freelb(lb);
				exhausted("memory");
			}
			lb->link[chan].nodrop = 1;
		}
	}
	poperror();
	qunlock(lb);

	c->qid = (Qid){CHDIR|NETQID(2*lb->path, Qdir), 0};
	c->aux = lb;
	c->dev = 0;
	return c;
}

static Chan*
loopbackclone(Chan *c, Chan *nc)
{
	Loop *lb;
	int chan;

	lb = c->aux;
	nc = devclone(c, nc);
	qlock(lb);
	lb->ref++;
	if(c->flag & COPEN){
		switch(chan = NETTYPE(c->qid.path)){
		case Qdata0:
		case Qdata1:
			chan -= Qdata0;
			lb->link[chan].ref++;
			break;
		}
	}
	qunlock(lb);
	return nc;
}

static int
loopbackgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	Loop *lb;
	int id, len, chan;

	if(i == DEVDOTDOT){
		devdir(c, c->qid, "#X", 0, eve, CHDIR|0555, dp);
		return 1;
	}

	id = NETID(c->qid.path);
	if(i > 1)
		id++;
	if(tab==nil || i>=ntab)
		return -1;
	tab += i;
	lb = c->aux;
	switch(chan = tab->qid.path){
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		len = qlen(lb->link[chan].iq);
		break;
	default:
		len = tab->length;
		break;
	}
	devdir(c, (Qid){NETQID(id, tab->qid.path),0}, tab->name, len, eve, tab->perm, dp);
	return 1;
}


static int
loopbackwalk(Chan *c, char *name)
{
	return devwalk(c, name, loopbackdir, nelem(loopbackdir), loopbackgen);
}

static void
loopbackstat(Chan *c, char *db)
{
	Loop *lb;
	Dir dir;
	int chan;

	lb = c->aux;
	switch(chan = NETTYPE(c->qid.path)){
	case Qdir:
		devdir(c, c->qid, ".", nelem(loopbackdir)*DIRLEN, eve, CHDIR|0555, &dir);
		break;
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		devdir(c, c->qid, "data", qlen(lb->link[chan].iq), eve, 0660, &dir);
		break;
	default:
		panic("loopbackstat");
	}
	convD2M(&dir, db);
}

/*
 *  if the stream doesn't exist, create it
 */
static Chan*
loopbackopen(Chan *c, int omode)
{
	Loop *lb;
	int chan;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	lb = c->aux;
	qlock(lb);
	switch(chan = NETTYPE(c->qid.path)){
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		lb->link[chan].ref++;
		break;
	}
	qunlock(lb);

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
loopbackclose(Chan *c)
{
	Loop *lb;
	int ref, chan;

	lb = c->aux;

	qlock(lb);
	if(c->flag & COPEN){
		/*
		 *  closing either side hangs up the stream
		 */
		switch(chan = NETTYPE(c->qid.path)){
		case Qdata0:
		case Qdata1:
			chan -= Qdata0;
			if(--lb->link[chan].ref == 0){
				qhangup(lb->link[chan ^ 1].oq, nil);
				looper(lb);
			}
			break;
		}
	}


	/*
	 *  if both sides are closed, they are reusable
	 */
	if(lb->link[0].ref == 0 && lb->link[1].ref == 0){
		for(chan = 0; chan < 2; chan++){
			closelink(&lb->link[chan], 0);
			qreopen(lb->link[chan].iq);
			qreopen(lb->link[chan].oq);
		}
	}
	ref = --lb->ref;
	if(ref == 0)
		freelb(lb);
	qunlock(lb);
}

static void
freelb(Loop *lb)
{
	int chan;

	for(chan = 0; chan < 2; chan++)
		closelink(&lb->link[chan], 1);
}

/*
 * called with the Loop qlocked,
 * so only pushlink can mess with the queues
 */
static void
closelink(Link *link, int dofree)
{
	Queue *iq, *oq;
	Block *bp;

	ilock(link);
	iq = link->iq;
	oq = link->oq;
	bp = link->tq;
	link->tq = nil;
	link->tqtail = nil;
	link->tout = 0;
	link->tin = 0;
	iunlock(link);
	if(iq != nil){
		qclose(iq);
		if(dofree){
			ilock(link);
			free(iq);
			link->iq = nil;
			iunlock(link);
		}
	}
	if(oq != nil){
		qclose(oq);
		if(dofree){
			ilock(link);
			free(oq);
			link->oq = nil;
			iunlock(link);
		}
	}
	freeblist(bp);
}

static long
loopbackread(Chan *c, void *va, long n, vlong)
{
	Loop *lb;
	int chan;

	lb = c->aux;
//ZZZ ctl message to set q limit -- qsetlimit(q, limit)
//ZZZ ctl message to set blocking/dropping	qnoblock(q, dropit)
//ZZZ ctl message for delays
	switch(chan = NETTYPE(c->qid.path)){
	case Qdir:
		return devdirread(c, va, n, loopbackdir, nelem(loopbackdir), loopbackgen);
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		return qread(lb->link[chan].iq, va, n);
	default:
		panic("loopbackread");
	}
	return -1;	/* not reached */
}

static Block*
loopbackbread(Chan *c, long n, ulong offset)
{
	Loop *lb;
	int chan;

	lb = c->aux;
	switch(chan = NETTYPE(c->qid.path)){
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		return qbread(lb->link[chan].iq, n);
	}

	return devbread(c, n, offset);
}

static long
loopbackbwrite(Chan *c, Block *bp, ulong off)
{
	Loop *lb;
	int chan;

	lb = c->aux;
	switch(chan = NETTYPE(c->qid.path)){
	case Qdata0:
	case Qdata1:
		chan -= Qdata0;
		return loopoput(lb, &lb->link[chan ^ 1], bp);
	default:
		return devbwrite(c, bp, off);
	}
}

static long
loopbackwrite(Chan *c, void *va, long n, vlong off)
{
	Block *bp;

	if(!islo())
		print("loopbackwrite hi %lux\n", getcallerpc(&c));

	switch(NETTYPE(c->qid.path)){
	case Qdata0:
	case Qdata1:
		bp = allocb(n);
		if(waserror()){
			freeb(bp);
			nexterror();
		}
		memmove(bp->wp, va, n);
		poperror();
		bp->wp += n;
		return loopbackbwrite(c, bp, off);
	case Qctl:
	default:
		panic("loopbackwrite");
	}

	return n;
}

static long
loopoput(Loop *lb, Link *link, Block *bp)
{
	long n;

	n = BLEN(bp);

	/* make it a single block with space for the loopback header */
	bp = padblock(bp, TMSIZE);
	if(bp->next)
		bp = concatblock(bp);
	if(BLEN(bp) < lb->minmtu)
		bp = adjustblock(bp, lb->minmtu);

	qbwrite(link->oq, bp);
	looper(lb);
	return n;
}

/*
 * move blocks between queues if they are ready.
 * schedule an interrupt for the next interesting time
 */
static void
looper(Loop *lb)
{
	vlong t, tt;

	tt = fastticks(nil);
again:;
	t = pushlink(&lb->link[0], tt);
	tt = pushlink(&lb->link[1], tt);
	if(t > tt && tt)
		t = tt;
	if(t){
		tt = fastticks(nil);
		if(tt <= t)
			goto again;
		//schedule an intr at tt-t fastticks
	}
}

static vlong
pushlink(Link *link, vlong now)
{
	Block *bp;
	vlong t;

	/*
	 * put another block in the link queue
	 */
	ilock(link);
	if(link->iq == nil || link->oq == nil){
		iunlock(link);
		return 0;
	}
	t = link->tout;
	if(!t || t < now){
		bp = qget(link->oq);
		if(bp != nil){
			if(!t)
				t = now;
			link->tout = t + BLEN(bp) * link->delayn;
			ptime(bp->rp, t + link->delay0);
//ZZZ drop or introduce errors here
			if(link->tq == nil)
				link->tq = bp;
			else
				link->tqtail->next = bp;
			link->tqtail = bp;
		}else
			link->tout = 0;
	}

	/*
	 * put more blocks into the receive queue
	 */
	t = 0;
	while(bp = link->tq){
		t = gtime(bp->rp);
		if(t > now)
			break;
		bp->rp += TMSIZE;
		link->tq = bp->next;
		bp->next = nil;
		if(link->nodrop)
			qpassnolim(link->iq, bp);
		else if(qpass(link->iq, bp) < 0)
			link->soverflows++;
		t = 0;
	}
	if(bp == nil && qisclosed(link->oq) && !qcanread(link->oq) && !qisclosed(link->iq))
		qhangup(link->iq, nil);
	link->tin = t;
	if(!t || t < link->tout)
		t = link->tout;
	iunlock(link);
	return t;
}

static void
ptime(uchar *p, vlong t)
{
	ulong tt;

	tt = t >> 32;
	p[0] = tt >> 24;
	p[1] = tt >> 16;
	p[2] = tt >> 8;
	p[3] = tt;
	tt = t;
	p[4] = tt >> 24;
	p[5] = tt >> 16;
	p[6] = tt >> 8;
	p[7] = tt;
}

static vlong
gtime(uchar *p)
{
	ulong t1, t2;

	t1 = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
	t2 = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
	return ((vlong)t1 << 32) | t2;
}

Dev loopbackdevtab = {
	'X',
	"loopback",

	devreset,
	loopbackinit,
	loopbackattach,
	loopbackclone,
	loopbackwalk,
	loopbackstat,
	loopbackopen,
	devcreate,
	loopbackclose,
	loopbackread,
	loopbackbread,
	loopbackwrite,
	loopbackbwrite,
	devremove,
	devwstat,
};
