#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"../port/netif.h"

/*
 * Register set for half the duart. 
 * There are really two sets in adjacent memory locations.
 */
struct Duartreg
{
	uchar	mr12;		/* Mode Register Channels 1 & 2 */
	uchar	pad0[15];
	uchar	srcsr;		/* Status Register/Clock Select Register */
	uchar	pad1[15];
	uchar	cmnd;		/* Command Register */
	uchar	pad2[15];
	uchar	data;		/* RX Holding / TX Holding Register */
	uchar	pad3[15];
	uchar	ipcacr;		/* Input Uart Change/Aux. Control Register */
	uchar	pad4[15];
	uchar	isimr;		/* Interrupt Status/Interrupt Mask Register */
	uchar	pad5[15];
	uchar	ctur;		/* Counter/Timer Upper Register */
	uchar	pad6[15];
	uchar	ctlr;		/* Counter/Timer Lower Register */
	uchar	pad7[15];
};
#define	ppcr	isimr		/* in the second register set */

enum
{
	DBD75		= 0,
	DBD110		= 1,
	DBD38400	= 2,
	DBD150		= 3,
	DBD300		= 4,
	DBD600		= 5,
	DBD1200		= 6,
	DBD2000		= 7,
	DBD2400		= 8,
	DBD4800		= 9,
	DBD1800		= 10,
	DBD9600		= 11,
	DBD19200	= 12,
	CHARERR		= 0x00,	/* MR1x - Mode Register 1 */
	EVENPAR		= 0x00,
	ODDPAR		= 0x04,
	NOPAR		= 0x10,
	CBITS8		= 0x03,
	CBITS7		= 0x02,
	CBITS6		= 0x01,
	CBITS5		= 0x00,
	NORMOP		= 0x00,	/* MR2x - Mode Register 2 */
	TWOSTOPB	= 0x0F,
	ONESTOPB	= 0x07,
	ENBRX		= 0x01,	/* CRx - Command Register */
	DISRX		= 0x02,
	ENBTX		= 0x04,
	DISTX		= 0x08,
	RESETMR 	= 0x10,
	RESETRCV  	= 0x20,
	RESETTRANS  	= 0x30,
	RESETERR  	= 0x40,
	RESETBCH	= 0x50,
	STRTBRK		= 0x60,
	STOPBRK		= 0x70,
	RCVRDY		= 0x01,	/* SRx - Channel Status Register */
	FIFOFULL	= 0x02,
	XMTRDY		= 0x04,
	XMTEMT		= 0x08,
	OVRERR		= 0x10,
	PARERR		= 0x20,
	FRMERR		= 0x40,
	RCVDBRK		= 0x80,
	IMIPC		= 0x80,	/* IMRx/ISRx - Int Mask/Interrupt Status */
	IMDBB		= 0x40,
	IMRRDYB		= 0x20,
	IMXRDYB		= 0x10,
	IMCRDY		= 0x08,
	IMDBA		= 0x04,
	IMRRDYA		= 0x02,
	IMXRDYA		= 0x01,
	BD38400		= 0xCC|0x0000,
	BD19200		= 0xCC|0x0100,
	BD9600		= 0xBB|0x0000,
	BD4800		= 0x99|0x0000,
	BD2400		= 0x88|0x0000,
	BD1200		= 0x66|0x0000,
	BD300		= 0x44|0x0000,

	Maxduart	= 8,
};

/*
 *  requests to perform on a duart
 */
enum
{
	Dnone=	0,
	Dbaud,
	Dbreak,
	Ddtr,
	Dprint,
	Dena,
	Dstate,
};

/*
 *  a duart
 */
typedef struct Duart	Duart;
struct Duart
{
	QLock;
	Duartreg	*reg;		/* duart registers */
	uchar		imr;		/* sticky interrupt mask reg bits */
	uchar		acr;		/* sticky auxiliary reg bits */
	int		inited;
};
Duart duart[Maxduart];

/*
 *  values specific to a single duart port
 */
typedef struct Uart	Uart;
struct Uart
{
	QLock;
	Duart		*d;		/* device */
	Duartreg	*reg;		/* duart registers (for this port) */
	int		c;		/* character to restart output */
	int		op;		/* operation requested */
	int		val;		/* value of operation */
	Rendez		opr;		/* waiot here for op to complete */

	int		printing;	/* need kick */
	int		opens;
	Rendez		r;

	/* buffers */
	int	(*putc)(Queue*, int);
	Queue	*iq;
	Queue	*oq;
};
Uart uart[2*Maxduart];

void	duartkick(Uart*);

/*
 *  configure a duart port, default is 9600 baud, 8 bits/char, 1 stop bit,
 *  no parity
 */
void
duartsetup(Uart *p, Duart *d, int devno)
{
	Duartreg *reg;

	p->d = d;
	reg = &d->reg[devno];
	p->reg = reg;

	reg->cmnd = RESETRCV|DISTX|DISRX;
	reg->cmnd = RESETTRANS;
	reg->cmnd = RESETERR;
	reg->cmnd = STOPBRK;

	reg->cmnd = RESETMR;
	reg->mr12 = NOPAR|CBITS8;
	reg->mr12 = ONESTOPB;
	reg->srcsr = (DBD9600<<4)|DBD9600;
	reg->cmnd = ENBTX|ENBRX;

	p->iq = qopen(4*1024, 0, 0, 0);
	p->oq = qopen(4*1024, 0, duartkick, p);
}

/*
 *  init the duart on the current processor
 */
void
duartinit(void)
{
	Uart *p;
	Duart *d;

	d = &duart[m->machno];
	if(d->inited)
		return;

	d->reg = DUARTREG;
	d->imr = IMRRDYA|IMXRDYA|IMRRDYB|IMXRDYB;
	d->reg->isimr = d->imr;
	d->acr = 0x80;			/* baud rate set 2 */
	d->reg->ipcacr = d->acr;

	p = &uart[2*m->machno];

	duartsetup(p, d, 0);
	p++;
	duartsetup(p, d, 1);
	d->inited = 1;
}

/*
 *  enable a duart port
 */
void
duartenable(Uart *p)
{
	p->reg->cmnd = ENBTX|ENBRX;
}

void
duartenable0(void)
{
	DUARTREG->cmnd = ENBTX|ENBRX;
}

void
duartbaud(Uart *p, int b)
{
	int x;

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
		p->d->acr |= 0x80;
	else
		p->d->acr &= ~0x80;
	p->d->reg->ipcacr = p->d->acr;
	p->reg->srcsr = x;
}

void
duartdtr(Uart *p, int val)
{
	if (val)
		p->reg->ctlr = 0x01;
	else
		p->reg->ctur = 0x01;
}

void
duartbreak(Uart *p, int val)
{
	Duartreg *reg;

	reg = p->reg;
	if (val){
		p->d->imr &= ~IMXRDYB;
		p->d->reg->isimr = p->d->imr;
		reg->cmnd = STRTBRK|ENBTX;
	} else {
		reg->cmnd = STOPBRK|ENBTX;
		p->d->imr |= IMXRDYB;
		p->d->reg->isimr = p->d->imr;
	}
}

/*
 *  do anything requested for this CPU's duarts
 */
void
duartslave0(Uart *p)
{
	switch(p->op){
	case Ddtr:
		duartdtr(p, p->val);
		break;
	case Dbaud:
		duartbaud(p, p->val);
		break;
	case Dbreak:
		duartbreak(p, p->val);
		break;
	case Dprint:
		p->reg->cmnd = ENBTX;
		p->reg->data = p->val;
		break;
	case Dena:
		duartenable(p);
		break;
	case Dstate:
		p->val = p->reg->ppcr;
		break;
	}
	p->op = Dnone;
	wakeup(&p->opr);
}
void
duartslave(void)
{
	Uart *p;

	p = &uart[2*m->machno];
	if(p->op != Dnone)
		duartslave0(p);
	p++;
	if(p->op != Dnone)
		duartslave0(p);
}

void
duartrintr(Uart *p)
{
	char ch;
	int status;
	Duartreg *reg;

	reg = p->reg;
	status = reg->srcsr;
	ch = reg->data;
	if(status & (FRMERR|OVRERR|PARERR))
		reg->cmnd = RESETERR;

	if(p->putc)
		(*p->putc)(p->iq, ch);
	else
		qproduce(p->iq, &ch, 1);
}

/*
 *  (re)start output
 */
void
duartkick(Uart *p)
{
	char ch;
	int n, x;

	x = splhi();
	if(p->printing) {
		splx(x);
		return;
	}

	n = qconsume(p->oq, &ch, 1);
	if(n <= 0){
		splx(x);
		return;
	}

	p->printing = 1;
	p->val = ch;
	p->op = Dprint;
	splx(x);
}

void
duartxintr(Uart *p)
{
	char ch;
	Duartreg *reg;

	reg = p->reg;
	if(qconsume(p->oq, &ch, 1) <= 0) {
		p->printing = 0;
		reg->cmnd = DISTX;
	}
	else
		reg->data = ch;
}

void
duartintr(void)
{
	int cause;
	Duartreg *reg;
	Uart *p;

	p = &uart[2*m->machno];
	reg = p->reg;
	cause = reg->isimr;

	if(cause & IMRRDYA)
		duartrintr(p);

	if(cause & IMXRDYA)
		duartxintr(p);

	if(cause & IMRRDYB)
		duartrintr(p+1);

	if(cause & IMXRDYB)
		duartxintr(p+1);
}

/*
 *  processor 0 only
 */
int
duartrawputc(int c)
{
	int i;
	Duartreg *reg;

	reg = DUARTREG;
	if(c == '\n') {
		duartrawputc('\r');
		delay(100);
	}
	reg->cmnd = ENBTX;
	i = 0;
	while((reg->srcsr&XMTRDY) == 0 && i++ < 100000)
		;
	reg->data = c;
	return c;
}

int
iprint(char *fmt, ...)
{
	int n, i;
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	n = doprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	for(i = 0; i < n; i++)
		duartrawputc(buf[i]);
	return n;
}

void
duartspecial(int port, int s, Queue **in, Queue **out, int (*putc)(Queue*, int))
{
	Uart *p;

	p = &uart[port];

	duartenable(p);
	if(s)
		duartbaud(p, s);

	p->putc = putc;
	if(in != 0)
		*in = p->iq;
	if(out != 0)
		*out = p->oq;

	p->opens++;
}

static int
opdone(void *x)
{
	Uart *p = x;

	return p->op == Dnone;
}

Dirtab *duartdir;
int nuart;

void
duartreset(void)
{
	int i;
	Dirtab *dp;

	nuart = 2*conf.nmach;
	duartdir = xalloc(2 * nuart * sizeof(Dirtab));
	dp = duartdir;
	for(i = 0; i < nuart; i++){
		/* 2 directory entries per port */
		print(dp->name, "eia%d", i);
		dp->qid.path = NETQID(i, Ndataqid);
		dp->perm = 0666;
		dp++;

		print(dp->name, "eia%dctl", i);
		dp->qid.path = NETQID(i, Nctlqid);
		dp->perm = 0666;
		dp++;
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
	return devwalk(c, name, duartdir, 2*nuart, devgen);
}

void
duartstat(Chan *c, char *dp)
{
	int i;
	Uart *p;
	Dir dir;

	i = NETID(c->qid.path);
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		p = &uart[i];
		devdir(c, c->qid, duartdir[2*i].name, qlen(p->iq), eve, 0660, &dir);
		convD2M(&dir, dp);
		break;
	default:
		devstat(c, dp, duartdir, 2*nuart, devgen);
		break;
	}
}

Chan*
duartopen(Chan *c, int omode)
{
	Uart *p;

	if(c->qid.path & CHDIR){
		if(omode != OREAD)
			error(Ebadarg);
	} 
	else {
		p = &uart[NETID(c->qid.path)];
		qlock(p);
		p->opens++;
		if(p->opens == 1) {
			/* enable the port */
			p->op = Dena;
			sleep(&p->opr, opdone, p);
		
			qreopen(p->iq);
			qreopen(p->oq);
		}
		qunlock(p);
	}

	c->mode = omode&~OTRUNC;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
duartcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
duartclose(Chan *c)
{
	Uart *p;

	if(c->qid.path & CHDIR)
		return;

	p = &uart[NETID(c->qid.path)];
	qlock(p);
	p->opens++;
	if(p->opens == 0){
		qclose(p->iq);
		qclose(p->oq);
	}
	qunlock(p);
}

long
duartread(Chan *c, void *buf, long n, ulong offset)
{
	Uart *p;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, duartdir, 2*nuart, devgen);

	p = &uart[NETID(c->qid.path)];
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qread(p->iq, buf, n);
	case Nctlqid:
		return readnum(offset, buf, n, NETID(c->qid.path), NUMSIZE);
	}

	return 0;
}

Block*
duartbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static void
duartctl(Uart *p, char *cmd)
{
	int n, i;

	/* let output drain for a while */
	for(i = 0; i < 16 && qlen(p->oq); i++)
		tsleep(&p->r, qlen, p->oq, 125);

	n = atoi(cmd+1);
	switch(cmd[0]){
	case 'B':
	case 'b':
		if(strncmp(cmd+1, "reak", 4) == 0)
			break;
		p->val = n;
		p->op = Dbaud;
		sleep(&p->opr, opdone, p);
		break;
	case 'D':
	case 'd':
		p->val = n;
		p->op = Ddtr;
		sleep(&p->opr, opdone, p);
		break;
	case 'K':
	case 'k':
		p->val = 1;
		p->op = Dbreak;
		if(!waserror()){
			sleep(&p->opr, opdone, p);
			tsleep(&p->opr, return0, 0, n);
			poperror();
		}
		p->val = 0;
		p->op = Dbreak;
		sleep(&p->opr, opdone, p);
		break;
	case 'R':
	case 'r':
		/* can't control? */
		break;
	}
}

long
duartwrite(Chan *c, void *va, long n, ulong offset)
{
	Uart *p;
	char cmd[32];

	USED(offset);

	if(c->qid.path & CHDIR)
		error(Eperm);

	p = &uart[NETID(c->qid.path)];
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qwrite(p->oq, va, n);
	case Nctlqid:
		if(n >= sizeof(cmd))
			n = sizeof(cmd)-1;
		memmove(cmd, va, n);
		cmd[n] = 0;
		duartctl(p, cmd);
		return n;
	}
}

long
duartbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void
duartremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
duartwstat(Chan *c, char *p)
{
	USED(c, p);
	error(Eperm);
}
