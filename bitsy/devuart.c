#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"../port/netif.h"

enum
{
	Nuart = 1,
	Stagesize= 1024,
};

/* hardware registers */
struct Uartregs
{
	ulong	ctl[4];
	ulong	dummya;
	ulong	data;
	ulong	dummyb;
	ulong	status0;
	ulong	status1;
};

enum
{
	/* ctl[0] bits */
	Parity=		1<<0,
	Even=		1<<1,
	Stop1=		0<<2,
	Stop2=		1<<2,
	Bits7=		0<<3,
	Bits8=		1<<3,
	SCE=		1<<4,	/* synchronous clock enable */
	RCE=		1<<5,	/* rx on falling edge of clock */
	TCE=		1<<6,	/* tx on falling edge of clock */

	/* ctl[3] bits */
	Rena=		1<<0,	/* receiver enable */
	Tena=		1<<1,	/* transmitter enable */
	Break=		1<<2,	/* force TXD3 low */
	Rintena=	1<<3,	/* enable receive interrupt */
	Tintena=	1<<4,	/* enable transmitter interrupt */
	Loopback=	1<<5,	/* loop back data */

	/* data bits */
	DEparity=	1<<8,	/* parity error */
	DEframe=		1<<9,	/* framing error */
	DEoverrun=	1<<10,	/* overrun error */

	/* status0 bits */
	Tint=		1<<0,	/* transmit fifo half full interrupt */
	Rint0=		1<<1,	/* receiver fifo 1/3-2/3 full */
	Rint1=		1<<2,	/* receiver fifo not empty and receiver idle */
	Breakstart=	1<<3,
	Breakend=	1<<4,
	Fifoerror=	1<<5,	/* fifo error */

	/* status1 bits */
	Tbusy=		1<<0,	/* transmitting */
	Rnotempty=	1<<1,	/* receive fifo not empty */
	Tnotfull=	1<<2,	/* transmit fifo not full */
	ParityError=	1<<3,
	FrameError=	1<<4,
	Overrun=	1<<5,
};

Uartregs *uart3regs = UART3REGS;

/* software representation */
typedef struct Uart Uart;
struct Uart
{
	QLock;
	int	dev;
	int	opens;
	Uartregs	*regs;

	int	enabled;
	Uart	*elist;			/* next enabled interface */
	char	name[NAMELEN];

	uchar	sticky[4];		/* sticky write register values */
	ulong	freq;			/* clock frequency */
	uchar	mask;			/* bits/char */
	int	baud;			/* baud rate */

	int	parity;			/* parity errors */
	int	frame;			/* framing errors */
	int	overrun;		/* rcvr overruns */

	/* buffers */
	int	(*putc)(Queue*, int);
	Queue	*iq;
	Queue	*oq;

	Lock	rlock;			/* receive */
	uchar	istage[Stagesize];
	uchar	*ip;
	uchar	*ie;

	int	haveinput;

	Lock	tlock;			/* transmit */
	uchar	ostage[Stagesize];
	uchar	*op;
	uchar	*oe;

	int	modem;			/* hardware flow control on */
	int	xonoff;			/* software flow control on */
	int	blocked;
	int	cts, dsr, dcd, dcdts;		/* keep track of modem status */ 
	int	ctsbackoff;
	int	hup_dsr, hup_dcd;	/* send hangup upstream? */
	int	dohup;

	int	kinuse;		/* device in use by kernel */

	Rendez	r;
};
static	Uart*	uart[Nuart];
static	int	nuart;

static Dirtab *uartdir;
static int uartndir;

/*
 * means the kernel is using this as a console
 */
static char	Ekinuse[] = "device in use by kernel";

static void	uartsetbaud(Uart *p, int rate);

/*
 *  define a Uart.
 */
static void
uartsetup(Uartregs *regs, ulong freq, char *name)
{
	Uart *p;

	if(nuart >= Nuart)
		return;

	p = xalloc(sizeof(Uart));
	uart[nuart] = p;
	strcpy(p->name, name);
	p->dev = nuart++;
	p->port = port;
	p->freq = freq;
	p->regs = regs;

	memset(p->sticky, 0, sizeof(p->sticky));

	/*
	 *  set rate to 115200 baud.
	 *  8 bits/character.
	 *  1 stop bit.
	 *  interrupts disabled.
	 */
	p->sticky[0] = Bits8;
	p->regs->ctl[0] = p->sticky[0];
	p->sticky[3] = Rena|Tena;
	p->regs->ctl[3] = p->sticky[3];
	uartsetbaud(p, 115200);

	p->iq = qopen(4*1024, 0, uartflow, p);
	p->oq = qopen(4*1024, 0, uartkick, p);
	if(p->iq == nil || p->oq == nil)
		panic("uartsetup");

	p->ip = p->istage;
	p->ie = &p->istage[Stagesize];
	p->op = p->ostage;
	p->oe = p->ostage;
}

/*
 *  enable a port's interrupts.  set DTR and RTS
 */
static void
uartenable(Uart *p)
{
	p->sticky[3] |= Rintena|Tintena;
	p->regs->ctl[3] = p->sticky[3];
}

/*
 *  disable interrupts. clear DTR, and RTS
 */
static void
uartdisable(Uart *p)
{
	p->sticky[3] &= ~(Rintena|Tintena);
	p->regs->ctl[3] = p->sticky[3];
}

static long
uartstatus(Chan*, Uart *p, void *buf, long n, long offset)
{
	USED(p);
//		"b%d c%d d%d e%d l%d m%d p%c r%d s%d i%d\n"
//		"dev(%d) type(%d) framing(%d) overruns(%d)%s%s%s%s\n",
	return readstr(offset, buf, n, "");
}

static void
setlength(int i)
{
	Uart *p;

	if(i > 0){
		p = uart[i];
		if(p && p->opens && p->iq)
			uartdir[3*i].length = qlen(p->iq);
	} else for(i = 0; i < nuart; i++){
		p = uart[i];
		if(p && p->opens && p->iq)
			uartdir[3*i].length = qlen(p->iq);
	}
}

/*
 *  setup the '#t' directory
 */
static void
uartreset(void)
{
	int i;
	Dirtab *dp;

	uartsetup(uart3regs, ;

	uartndir = 3*nuart;
	uartdir = xalloc(uartndir * sizeof(Dirtab));
	dp = uartdir;
	for(i = 0; i < nuart; i++){
		/* 3 directory entries per port */
		sprint(dp->name, "eia%d", i);
		dp->qid.path = NETQID(i, Ndataqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dctl", i);
		dp->qid.path = NETQID(i, Nctlqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dstat", i);
		dp->qid.path = NETQID(i, Nstatqid);
		dp->perm = 0444;
		dp++;
	}

}


static Chan*
uartattach(char *spec)
{
	return devattach('t', spec);
}

static int
uartwalk(Chan *c, char *name)
{
	return devwalk(c, name, uartdir, uartndir, devgen);
}

static void
uartstat(Chan *c, char *dp)
{
	if(NETTYPE(c->qid.path) == Ndataqid)
		setlength(NETID(c->qid.path));
	devstat(c, dp, uartdir, uartndir, devgen);
}

static Chan*
uartopen(Chan *c, int omode)
{
	Uart *p;

	c = devopen(c, omode, uartdir, uartndir, devgen);

	switch(NETTYPE(c->qid.path)){
	case Nctlqid:
	case Ndataqid:
		p = uart[NETID(c->qid.path)];
		if(p->kinuse)
			error(Ekinuse);
		qlock(p);
		if(p->opens++ == 0){
			uartenable(p);
			qreopen(p->iq);
			qreopen(p->oq);
		}
		qunlock(p);
		break;
	}

	return c;
}

static void
uartclose(Chan *c)
{
	Uart *p;

	if(c->qid.path & CHDIR)
		return;
	if((c->flag & COPEN) == 0)
		return;
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
	case Nctlqid:
		p = uart[NETID(c->qid.path)];
		if(p->kinuse)
			error(Ekinuse);
		qlock(p);
		if(--(p->opens) == 0){
			uartdisable(p);
			qclose(p->iq);
			qclose(p->oq);
			p->ip = p->istage;
			p->dcd = p->dsr = p->dohup = 0;
		}
		qunlock(p);
		break;
	}
}

static long
uartread(Chan *c, void *buf, long n, vlong off)
{
	Uart *p;
	ulong offset = off;

	if(c->qid.path & CHDIR){
		setlength(-1);
		return devdirread(c, buf, n, uartdir, uartndir, devgen);
	}

	p = uart[NETID(c->qid.path)];
	if(p->kinuse)
		error(Ekinuse);
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qread(p->iq, buf, n);
	case Nctlqid:
		return readnum(offset, buf, n, NETID(c->qid.path), NUMSIZE);
	case Nstatqid:
		return uartstatus(c, p, buf, n, offset);
	}

	return 0;
}

static void
uartctl(Uart *p, char *cmd)
{
	int i, n;
	char *f[32];
	int nf;

	/* let output drain for a while */
	for(i = 0; i < 16 && qlen(p->oq); i++)
		tsleep(&p->r, (int(*)(void*))qlen, p->oq, 125);

	nf = getfields(cmd, f, nelem(f), 1, " \t\n");

	for(i = 0; i < nf; i++){

		if(strncmp(f[i], "break", 5) == 0){
//			uartbreak(p, 0);
			continue;
		}

		n = atoi(f[i]+1);
		switch(*f[i]){
		case 'B':
		case 'b':
//			uartsetbaud(p, n);
			break;
		case 'C':
		case 'c':
//			uartdcdhup(p, n);
			break;
		case 'D':
		case 'd':
//			uartdtr(p, n);
			break;
		case 'E':
		case 'e':
//			uartdsrhup(p, n);
			break;
		case 'f':
		case 'F':
			qflush(p->oq);
			break;
		case 'H':
		case 'h':
			qhangup(p->iq, 0);
			qhangup(p->oq, 0);
			break;
		case 'i':
		case 'I':
//			lock(&p->flock);
//			uartfifo(p, n);
//			unlock(&p->flock);
			break;
		case 'L':
		case 'l':
//			uartbits(p, n);
			break;
		case 'm':
		case 'M':
//			uartmflow(p, n);
			break;
		case 'n':
		case 'N':
			qnoblock(p->oq, n);
			break;
		case 'P':
		case 'p':
//			uartparity(p, *(cmd+1));
			break;
		case 'K':
		case 'k':
//			uartbreak(p, n);
			break;
		case 'R':
		case 'r':
//			uartrts(p, n);
			break;
		case 'Q':
		case 'q':
			qsetlimit(p->iq, n);
			qsetlimit(p->oq, n);
			break;
		case 'T':
		case 't':
//			uartdcdts(p, n);
			break;
		case 'W':
		case 'w':
			/* obsolete */
			break;
		case 'X':
		case 'x':
			ilock(&p->tlock);
			p->xonoff = n;
			iunlock(&p->tlock);
			break;
		}
	}
}

static long
uartwrite(Chan *c, void *buf, long n, vlong)
{
	Uart *p;
	char cmd[32];

	if(c->qid.path & CHDIR)
		error(Eperm);

	p = uart[NETID(c->qid.path)];
	if(p->kinuse)
		error(Ekinuse);

	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qwrite(p->oq, buf, n);
	case Nctlqid:
		if(n >= sizeof(cmd))
			n = sizeof(cmd)-1;
		memmove(cmd, buf, n);
		cmd[n] = 0;
		uartctl(p, cmd);
		return n;
	}
}

static void
uartwstat(Chan *c, char *dp)
{
	Dir d;
	Dirtab *dt;

	if(!iseve())
		error(Eperm);
	if(CHDIR & c->qid.path)
		error(Eperm);
	if(NETTYPE(c->qid.path) == Nstatqid)
		error(Eperm);

	dt = &uartdir[3 * NETID(c->qid.path)];
	convM2D(dp, &d);
	d.mode &= 0666;
	dt[0].perm = dt[1].perm = d.mode;
}

Dev uartdevtab = {
	't',
	"uart",

	uartreset,
	devinit,
	uartattach,
	devclone,
	uartwalk,
	uartstat,
	uartopen,
	devcreate,
	uartclose,
	uartread,
	devbread,
	uartwrite,
	devbwrite,
	devremove,
	uartwstat,
};

static void
uartsetbaud(Uart *p, int rate)
{
	ulong brconst;

	if(rate <= 0)
		return;

	brconst = (p->freq+8*rate-1)/(16*rate) - 1;
	p->regs->ctl[1] = (brconst>>8) & 0xf;
	p->regs->ctl[2] = brconst;

	p->baud = rate;
}

void
serialputs(char *str, int n)
{
	Uartregs *ur;

	ur = uart3regs;
	while(n-- > 0){
		/* wait for output ready */
		while((ur->status1 & Tnotfull) == 0)
			;
		ur->data = *str++;
	}
	while((ur->status1 & Tbusy))
		;
}
