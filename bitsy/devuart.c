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
	ulong	ctl0;
	ulong	ctl1;
	ulong	ctl2;
	ulong	ctl3;
	ulong	dummya;
	ulong	data;
	ulong	dummyb;
	ulong	status0;
	ulong	status1;
};

Uartregs *uart3regs = UART3REGS;

/* ctl0 bits */
enum
{
	/* status register 1 bits */
	XmitBusy = (1 << 0),
	XmitNotFull = (1 << 2),
};

/* software representation */
typedef struct Uart Uart;
struct Uart
{
	QLock;
	int	opens;

	int	enabled;
	Uart	*elist;			/* next enabled interface */
	char	name[NAMELEN];

	uchar	sticky[8];		/* sticky write register values */
	uchar	osticky[8];		/* kernel saved sticky write register values */
	ulong	port;			/* io ports */
	ulong	freq;			/* clock frequency */
	uchar	mask;			/* bits/char */
	int	dev;
	int	baud;			/* baud rate */

	uchar	istat;			/* last istat read */
	int	frame;			/* framing errors */
	int	overrun;		/* rcvr overruns */

	/* buffers */
	int	(*putc)(Queue*, int);
	Queue	*iq;
	Queue	*oq;

	Lock	flock;			/* fifo */
	uchar	fifoon;			/* fifo's enabled */
	uchar	type;			/* chip version */

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
 * means the kernel is using this for debugging output
 */
static char	Ekinuse[] = "device in use by kernel";

/*
 *  default is 9600 baud, 1 stop bit, 8 bit chars, no interrupts,
 *  transmit and receive enabled, interrupts disabled.
 */
static void
uartsetup0(Uart *p)
{
	memset(p->sticky, 0, sizeof(p->sticky));
	/*
	 *  set rate to 9600 baud.
	 *  8 bits/character.
	 *  1 stop bit.
	 *  interrupts enabled.
	 */
//	p->sticky[Format] = Bits8;
//	uartwrreg(p, Format, 0);
//	p->sticky[Mctl] |= Inton;
//	uartwrreg(p, Mctl, 0x0);

//	uartsetbaud(p, 9600);

//	p->iq = qopen(4*1024, 0, uartflow, p);
//	p->oq = qopen(4*1024, 0, uartkick, p);
	if(p->iq == nil || p->oq == nil)
		panic("uartsetup0");

	p->ip = p->istage;
	p->ie = &p->istage[Stagesize];
	p->op = p->ostage;
	p->oe = p->ostage;
}

/*
 *  called by main() to create a new duart
 */
void
uartsetup(ulong port, ulong freq, char *name, int type)
{
	Uart *p;

	if(nuart >= Nuart)
		return;

	p = xalloc(sizeof(Uart));
	uart[nuart] = p;
	strcpy(p->name, name);
	p->dev = nuart;
	nuart++;
	p->port = port;
	p->freq = freq;
	p->type = type;
	uartsetup0(p);
}

static void
uartenable(Uart *p)
{
	USED(p);
}

static void
uartdisable(Uart *p)
{
	USED(p);
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

static void
uartreset(void)
{
	int i;
	Dirtab *dp;

	nuart = Nuart;

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

void
serialputs(char *str, int n)
{
	Uartregs *ur;

	ur = uart3regs;
	while(n-- > 0){
		/* wait for output ready */
		while((ur->status1 & XmitNotFull) == 0)
			;
		ur->data = *str++;
	}
	while((ur->status1 & XmitBusy))
		;
}
