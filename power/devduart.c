#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

int	duartacr;
int	duartimr;

void	duartputs(IOQ*, char*, int);
void	iprint(char*, ...);

#define	PAD	15	/* registers are well-spaced */

/*
 * Register set for half the duart.  There are really two sets.
 */
struct Duart{
	uchar	mr1_2,		/* Mode Register Channels 1 & 2 */
		pad0[PAD];
	uchar	sr_csr,		/* Status Register/Clock Select Register */
		pad1[PAD];
	uchar	cmnd,		/* Command Register */
		pad2[PAD];
	uchar	data,		/* RX Holding / TX Holding Register */
		pad3[PAD];
	uchar	ipc_acr,	/* Input Port Change/Aux. Control Register */
		pad4[PAD];
	uchar	is_imr,		/* Interrupt Status/Interrupt Mask Register */
		pad5[PAD];
	uchar	ctur,		/* Counter/Timer Upper Register */
		pad6[PAD];
	uchar	ctlr,		/* Counter/Timer Lower Register */
		pad7[PAD];
};
#define	ppcr	is_imr		/* in the second register set */

#define DBD75		0
#define DBD110		1
#define DBD38400	2
#define DBD150		3
#define DBD300		4
#define DBD600		5
#define DBD1200		6
#define DBD2000		7
#define DBD2400		8
#define DBD4800		9
#define DBD1800		10
#define DBD9600		11
#define DBD19200	12

enum{
	CHAR_ERR	=0x00,	/* MR1x - Mode Register 1 */
	EVEN_PAR	=0x00,
	ODD_PAR		=0x04,
	NO_PAR		=0x10,
	CBITS8		=0x03,
	CBITS7		=0x02,
	CBITS6		=0x01,
	CBITS5		=0x00,
	NORM_OP		=0x00,	/* MR2x - Mode Register 2 */
	TWOSTOPB	=0x0F,
	ONESTOPB	=0x07,
	ENB_RX		=0x01,	/* CRx - Command Register */
	DIS_RX		=0x02,
	ENB_TX		=0x04,
	DIS_TX		=0x08,
	RESET_MR 	=0x10,
	RESET_RCV  	=0x20,
	RESET_TRANS  	=0x30,
	RESET_ERR  	=0x40,
	RESET_BCH	=0x50,
	STRT_BRK	=0x60,
	STOP_BRK	=0x70,
	RCV_RDY		=0x01,	/* SRx - Channel Status Register */
	FIFOFULL	=0x02,
	XMT_RDY		=0x04,
	XMT_EMT		=0x08,
	OVR_ERR		=0x10,
	PAR_ERR		=0x20,
	FRM_ERR		=0x40,
	RCVD_BRK	=0x80,
	IM_IPC		=0x80,	/* IMRx/ISRx - Interrupt Mask/Interrupt Status */
	IM_DBB		=0x40,
	IM_RRDYB	=0x20,
	IM_XRDYB	=0x10,
	IM_CRDY		=0x08,
	IM_DBA		=0x04,
	IM_RRDYA	=0x02,
	IM_XRDYA	=0x01,
	BD38400		=0xCC|0x0000,
	BD19200		=0xCC|0x0100,
	BD9600		=0xBB|0x0000,
	BD4800		=0x99|0x0000,
	BD2400		=0x88|0x0000,
	BD1200		=0x66|0x0000,
	BD300		=0x44|0x0000,

	Maxport		=8,
};

/*
 *  requests to perform on a duart
 */
enum {
	Dnone=	0,
	Dbaud,
	Dbreak,
	Ddtr,
	Dprint,
	Dena,
	Dstate,
};

/*
 *  software info for a serial duart interface
 */
typedef struct Duartport	Duartport;
struct Duartport
{
	QLock;
	int	printing;	/* true if printing */
	Duart	*duart;		/* device */
	int	inited;
	int	c;		/* character to restart output */
	int	op;		/* operation requested */
	int	val;		/* value of operation */
	Rendez	opr;		/* waiot here for op to complete */

	/* console interface */
	int	nostream;	/* can't use the stream interface */
	IOQ	*iq;		/* input character queue */
	IOQ	*oq;		/* output character queue */

	/* stream interface */
	Queue	*wq;		/* write queue */
	Rendez	r;		/* kproc waiting for input */
	Alarm	*a;		/* alarm for waking the kernel process */
	int	delay;		/* between character input and waking kproc */
 	int	kstarted;	/* kproc started */
	uchar	delim[256/8];	/* characters that act as delimiters */
};
Duartport	duartport[Maxport];	/* max possible */

/*
 *  configure a duart port, default is 9600 baud, 8 bits/char, 1 stop bit,
 *  no parity
 */
void
duartsetup(Duartport *dp)
{
	Duart *duart;

	duart = dp->duart;

	duart->cmnd = RESET_RCV|DIS_TX|DIS_RX;
	duart->cmnd = RESET_TRANS;
	duart->cmnd = RESET_ERR;
	duart->cmnd = STOP_BRK;

	duart->ipc_acr = 0x80;		/* baud-rate set 2 */
	duart->cmnd = RESET_MR;
	duart->mr1_2 = NO_PAR|CBITS8;
	duart->mr1_2 = ONESTOPB;
	duart->sr_csr = (DBD9600<<4)|DBD9600;
	duart->is_imr = IM_RRDYA|IM_XRDYA;
	duart->cmnd = ENB_TX|ENB_RX;
}

/*
 *  init the duart on the current processor
 */
void
duartinit(void)
{
	Duartport *dp;

	dp = &duartport[2*m->machno];
	if(dp->inited)
		return;
	dp->inited = 1;

	dp->duart = DUARTREG;
	duartsetup(dp);
	dp++;
	dp->duart = DUARTREG+1;
	duartsetup(dp);
}

/*
 *  enable a duart port
 */
void
duartenable(Duartport *dp)
{
	dp->duart->cmnd = ENB_TX|ENB_RX;
}

void
duartenable0(void)
{
	DUARTREG->cmnd = ENB_TX|ENB_RX;
}

void
duartbaud(Duartport *dp, int b)
{
	int x = 0;

	switch(b){
	case 38400:
		x = BD38400;
		break;
	case 19200:
		x = BD19200;
		break;
	case 9600:
		x = BD9600;
		break;
	case 4800:
		x = BD4800;
		break;
	case 2400:
		x = BD2400;
		break;
	case 1200:
		x = BD1200;
		break;
	case 300:
		x = BD300;
		break;
	default:
		return;
	}
	if(x & 0x0100)
		dp->duart->ipc_acr = duartacr |= 0x80;
	else
		dp->duart->ipc_acr = duartacr &= ~0x80;
	dp->duart->sr_csr = x;
}

void
duartdtr(Duartport *dp, int val)
{
	if (val)
		dp->duart->ctlr=0x01;
	else
		dp->duart->ctur=0x01;
}

void
duartbreak(Duartport *dp, int val)
{
	Duart *duart;

	duart = dp->duart;
	if (val){
		duart->is_imr = duartimr &= ~IM_XRDYB;
		duart->cmnd = STRT_BRK|ENB_TX;
	} else {
		duart->cmnd = STOP_BRK|ENB_TX;
		duart->is_imr = duartimr |= IM_XRDYB;
	}
}

/*
 *  do anything requested for this CPU's duarts
 */
void
duartslave0(Duartport *dp)
{
	switch(dp->op){
	case Ddtr:
		duartbaud(dp, dp->val);
		break;
	case Dbaud:
		duartdtr(dp, dp->val);
		break;
	case Dbreak:
		duartbreak(dp, dp->val);
		break;
	case Dprint:
		dp->duart->cmnd = ENB_TX;
		dp->duart->data = dp->val;
		break;
	case Dena:
		duartenable(dp);
		break;
	case Dstate:
		dp->val = dp->duart->is_imr;
		break;
	}
	dp->op = Dnone;
	wakeup(&dp->opr);
}
void
duartslave(void)
{
	Duartport *dp;

	dp = &duartport[2*m->machno];
	if(dp->op != Dnone)
		duartslave0(dp);
	dp++;
	if(dp->op != Dnone)
		duartslave0(dp);
}

duartrintr(Duartport *dp)
{
	Duart *duart;
	IOQ *cq;
	int status;
	char ch;

	duart = dp->duart;
	status = duart->sr_csr;
	ch = duart->data;
	if(status & (FRM_ERR|OVR_ERR|PAR_ERR))
		duart->cmnd = RESET_ERR;

	cq = dp->iq;
	if(cq->putc)
		(*cq->putc)(cq, ch);
	else {
		putc(cq, ch);
		if(dp->delim[ch/8] & (1<<(ch&7)) )
			wakeup(&cq->r);
	}
}

duartxintr(Duartport *dp)
{
	Duart *duart;
	IOQ *cq;
	int ch;

	cq = dp->oq;
	lock(cq);
	ch = getc(cq);
	duart = dp->duart;
	if(ch < 0){
		dp->printing = 0;
		wakeup(&cq->r);
		duart->cmnd = DIS_TX;
	} else
		duart->data = ch;
	unlock(cq);
}

void
duartintr(void)
{
	int cause, status, c;
	Duart *duart;
	Duartport *dp;

	dp = &duartport[2*m->machno];
	duart = dp->duart;
	cause = duart->is_imr;
	/*
	 * I can guess your interrupt.
	 */
	/*
	 * Is it 1?
	 */
	if(cause & IM_RRDYA)
		duartrintr(dp);
	/*
	 * Is it 2?
	 */
	if(cause & IM_XRDYA)
		duartxintr(dp);
	/*
	 * Is it 3?
	 */
	if(cause & IM_RRDYB)
		duartrintr(dp+1);
	/*
	 * Is it 4?
	 */
	if(cause & IM_XRDYB)
		duartxintr(dp+1);
}

/*
 *  processor 0 only
 */
int
duartrawputc(int c)
{
	Duart *duart;
	int i;

	duart = DUARTREG;
	if(c == '\n')
		duartrawputc('\r');
	duart->cmnd = ENB_TX;
	i = 0;
	while((duart->sr_csr&XMT_RDY) == 0)
		if(++i >= 1000000){
			duartsetup(&duartport[0]);
			for(i=0; i<100000; i++)
				;
			break;
		}
	duart->data = c;
	if(c == '\n')
		for(i=0; i<100000; i++)
			;
	return c;
}
void
duartrawputs(char *s)
{
	int i;
	while(*s){
		duartrawputc(*s++);
	}
	for(i=0; i < 1000000; i++)
		;
}
void
iprint(char *fmt, ...)
{
	char buf[1024];
	long *arg;

	arg = (long*)(&fmt+1);
	sprint(buf, fmt, *arg, *(arg+1), *(arg+2), *(arg+3));
	duartrawputs(buf);
}

/*
 *  Queue n characters for output; if queue is full, we lose characters.
 *  Get the output going if it isn't already.
 */
void
duartputs(IOQ *cq, char *s, int n)
{
	int ch, x;
	Duartport *dp;
	Duart *duart;

	x = splhi();
	lock(cq);
	puts(cq, s, n);
	dp = cq->ptr;
	if(dp->printing == 0){
		ch = getc(cq);
		if(ch >= 0){
			dp->printing = 1;
			dp->val = ch;
			dp->op = Dprint;
		}
	}
	unlock(cq);
	splx(x);
}

/*
 *  set up an duart port as something other than a stream
 */
void
duartspecial(int port, IOQ *oq, IOQ *iq, int baud)
{
	Duartport *dp = &duartport[port];
	IOQ *zq;

	dp->nostream = 1;
	if(oq){
		dp->oq = oq;
		dp->oq->puts = duartputs;
		dp->oq->ptr = dp;
	}
	if(iq){
		dp->iq = iq;
		dp->iq->ptr = dp;

		/*
		 *  Stupid HACK to undo a stupid hack
		 */ 
		zq = &kbdq;
		if(iq == zq)
			kbdq.putc = kbdcr2nl;
	}
	duartenable(dp);
	duartbaud(dp, baud);
}

static void	duarttimer(Alarm*);
static int	duartputc(IOQ *, int);
static void	duartstopen(Queue*, Stream*);
static void	duartstclose(Queue*);
static void	duartoput(Queue*, Block*);
static void	duartkproc(void *);
Qinfo duartinfo =
{
	nullput,
	duartoput,
	duartstopen,
	duartstclose,
	"duart"
};

/*
 *  wakeup the helper process to do input
 */
static void
duarttimer(Alarm *a)
{
	Duartport *dp = a->arg;

	cancel(a);
	dp->a = 0;
	wakeup(&dp->iq->r);
}

static int
duartputc(IOQ *cq, int ch)
{
	Duartport *dp = cq->ptr; int r;

	r = putc(cq, ch);

	/*
	 *  pass upstream within dp->delay milliseconds
	 */
	if(dp->a==0){
		if(dp->delay == 0)
			wakeup(&cq->r);
		else
			dp->a = alarm(dp->delay, duarttimer, dp);
	}
	return r;
}

static void
duartstopen(Queue *q, Stream *s)
{
	Duartport *dp;
	char name[NAMELEN];

	dp = &duartport[s->id];

	qlock(dp);
	dp->wq = WR(q);
	WR(q)->ptr = dp;
	RD(q)->ptr = dp;
	dp->delay = 64;
	dp->iq->putc = duartputc;
	qunlock(dp);

	/* start with all characters as delimiters */
	memset(dp->delim, 1, sizeof(dp->delim));
	
	if(dp->kstarted == 0){
		dp->kstarted = 1;
		sprint(name, "duart%d", s->id);
		kproc(name, duartkproc, dp);
	}
}

static void
duartstclose(Queue *q)
{
	Duartport *dp = q->ptr;

	qlock(dp);
	dp->wq = 0;
	dp->iq->putc = 0;
	WR(q)->ptr = 0;
	RD(q)->ptr = 0;
	qunlock(dp);
}

static int
opdone(void *x)
{
	Duartport *dp = x;

	return dp->op == Dnone;
}
static void
duartoput(Queue *q, Block *bp)
{
	Duartport *dp = q->ptr;
	IOQ *cq;
	int n, m;

	if(dp == 0){
		freeb(bp);
		return;
	}
	cq = dp->oq;
	if(waserror()){
		freeb(bp);
		nexterror();
	}
	if(bp->type == M_CTL){
		qlock(dp);
		while (cangetc(cq))	/* let output drain */
			sleep(&cq->r, cangetc, cq);
		n = strtoul((char *)(bp->rptr+1), 0, 0);
		switch(*bp->rptr){
		case 'B':
		case 'b':
			dp->val = n;
			dp->op = Dbaud;
			sleep(&dp->opr, opdone, dp);
			break;
		case 'D':
		case 'd':
			dp->val = n;
			dp->op = Ddtr;
			sleep(&dp->opr, opdone, dp);
			break;
		case 'K':
		case 'k':
			dp->val = 1;
			dp->op = Dbreak;
			sleep(&dp->opr, opdone, dp);
			tsleep(&dp->opr, return0, 0, n);
			dp->val = 0;
			dp->op = Dbreak;
			sleep(&dp->opr, opdone, dp);
			break;
		case 'R':
		case 'r':
			/* can't control? */
			break;
		case 'W':
		case 'w':
			if(n>=0 && n<1000)
				dp->delay = n;
			break;
		}
		qunlock(dp);
	}else while((m = BLEN(bp)) > 0){
		while ((n = canputc(cq)) == 0){
			kprint(" duartoput: sleeping\n");
			sleep(&cq->r, canputc, cq);
		}
		if(n > m)
			n = m;
		(*cq->puts)(cq, bp->rptr, n);
		bp->rptr += n;
	}
	freeb(bp);
	poperror();
}

/*
 *  process to send bytes upstream for a port
 */
static void
duartkproc(void *a)
{
	Duartport *dp = a;
	IOQ *cq = dp->iq;
	Block *bp;
	int n;

loop:
	while ((n = cangetc(cq)) == 0)
		sleep(&cq->r, cangetc, cq);
	qlock(dp);
	if(dp->wq == 0){
		cq->out = cq->in;
	}else{
		bp = allocb(n);
		bp->flags |= S_DELIM;
		bp->wptr += gets(cq, bp->wptr, n);
		PUTNEXT(RD(dp->wq), bp);
	}
	qunlock(dp);
	goto loop;
}

Dirtab *duartdir;
int nduartport;

/*
 *  allocate the queues if no one else has
 */
void
duartreset(void)
{
	Duartport *dp;
	int i;

	/*
 	 *  allocate the directory and fill it in
	 */
	nduartport = 2*conf.nmach;
	duartdir = ialloc(nduartport*2*sizeof(Dirtab), 0);
	for(i = 0; i < nduartport; i++){
		sprint(duartdir[2*i].name, "eia%d", i);
		sprint(duartdir[2*i+1].name, "eia%dctl", i);
		duartdir[2*i].length = duartdir[2*i+1].length = 0;
		duartdir[2*i].perm = duartdir[2*i+1].perm = 0666;
		duartdir[2*i].qid.path = STREAMQID(i, Sdataqid);
		duartdir[2*i+1].qid.path = STREAMQID(i, Sctlqid);
	}

	/*
	 *  allocate queues for any stream interfaces
	 */
	for(dp = duartport; dp < &duartport[nduartport]; dp++){
		if(dp->nostream)
			continue;

		dp->iq = ialloc(sizeof(IOQ), 0);
		initq(dp->iq);
		dp->iq->ptr = dp;

		dp->oq = ialloc(sizeof(IOQ), 0);
		initq(dp->oq);
		dp->oq->ptr = dp;
		dp->oq->puts = duartputs;
	}
}

Chan*
duartattach(char *spec)
{
	return devattach('t', spec);
}

Chan*
duartclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
duartwalk(Chan *c, char *name)
{
	return devwalk(c, name, duartdir, 2*nduartport, devgen);
}

void
duartstat(Chan *c, char *dp)
{
	switch(STREAMTYPE(c->qid.path)){
	case Sdataqid:
		streamstat(c, dp, "eia0");
		break;
	default:
		devstat(c, dp, duartdir, 2*nduartport, devgen);
		break;
	}
}

Chan*
duartopen(Chan *c, int omode)
{
	Duartport *dp;

	switch(STREAMTYPE(c->qid.path)){
	case Sdataqid:
	case Sctlqid:
		dp = &duartport[STREAMID(c->qid.path)];
		break;
	default:
		dp = 0;
		break;
	}

	if(dp && dp->nostream)
		error(Einuse);

	if((c->qid.path & CHDIR) == 0)
		streamopen(c, &duartinfo);
	return devopen(c, omode, duartdir, 2*nduartport, devgen);
}

void
duartcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c);
	error(Eperm);
}

void
duartclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long
duartread(Chan *c, void *buf, long n, ulong offset)
{
	Duartport *dp;

	if(c->qid.path&CHDIR)
		return devdirread(c, buf, n, duartdir, 2*nduartport, devgen);

	switch(STREAMTYPE(c->qid.path)){
	case Sdataqid:
		return streamread(c, buf, n);
	case Sctlqid:
		if(offset)
			return 0;
		dp = &duartport[STREAMID(c->qid.path)];
		qlock(dp);
		dp->op = Dstate;
		sleep(&dp->opr, opdone, dp);
		*(uchar *)buf = dp->val;
		qunlock(dp);
		return 1;
	}

	error(Egreg);
}

long
duartwrite(Chan *c, void *va, long n, ulong offset)
{
	return streamwrite(c, va, n, 0);
}

void
duartremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
duartwstat(Chan *c, char *dp)
{
	USED(c);
	error(Eperm);
}

int
duartactive(void)
{
	int i;

	for(i = 0; i < nduartport; i++)
		if(duartport[i].printing)
			return 1;
	return 0;
}
