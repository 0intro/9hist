#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#include	"devtab.h"

/*
 *  Driver for an NS16450 serial port
 */
enum
{
	/*
	 *  register numbers
	 */
	Data=	0,		/* xmit/rcv buffer */
	Iena=	1,		/* interrupt enable */
	 Ircv=	(1<<0),		/*  interrupt on receive */
	 Ixmt=	(1<<1),		/*  interrupt on xmit */
	Istat=	2,		/* interrupt flag */
	Format=	3,		/* byte format */
	 Bits8=	(3<<0),		/*  8 bits/byte */
	 Stop2=	(1<<2),		/*  2 stop bits */
	 Pena=	(1<<3),		/*  generate parity */
	 Peven=	(1<<4),		/*  even parity */
	 Pforce=(1<<5),		/*  force parity */
	 Break=	(1<<6),		/*  generate a break */
	 Dra=	(1<<7),		/*  address the divisor */
	Mctl=	4,		/* modem control */
	 Dtr=	(1<<0),		/*  data terminal ready */
	 Rts=	(1<<1),		/*  request to send */
	 Ri=	(1<<2),		/*  ring */
	 Dcd=	(1<<3),		/*  carrier */
	 Loop=	(1<<4),		/*  loop bask */
	Lstat=	5,		/* line status */
	 Inready=(1<<0),	/*  receive buffer full */
	 Outbusy=(1<<5),	/*  output buffer full */
	Mstat=	6,		/* modem status */
	Scratch=7,		/* scratchpad */
	Dlsb=	0,		/* divisor lsb */
	Dmsb=	1,		/* divisor msb */
};

typedef struct Uart	Uart;
struct Uart
{
	QLock;
	int	port;
	ushort	sticky[16];	/* sticky write register values */
	int	printing;	/* true if printing */

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

Uart uart[2];

#define UartFREQ 1846200

#define uartwrreg(u,r,v)	outb(u->port + r, u->sticky[r] | v)
#define uartrdreg(u,r)		inb(u->port + r)

void	uartintr(Uart*);
void	uartintr0(Ureg*);
void	uartintr1(Ureg*);
void	uartsetup(void);

/*
 *  set the baud rate by calculating and setting the baudrate
 *  generator constant.  This will work with fairly non-standard
 *  baud rates.
 */
void
uartsetbaud(Uart *up, int rate)
{
	ulong brconst;

	brconst = (UartFREQ+8*rate-1)/16*rate;

	uartwrreg(up, Format, Dra);
	uartwrreg(up, Dmsb, (brconst>>8) & 0xff);
	uartwrreg(up, Dlsb, brconst & 0xff);
	uartwrreg(up, Format, 0);
}

/*
 *  toggle DTR
 */
void
uartdtr(Uart *up, int n)
{
	if(n)
		up->sticky[Mctl] |= Dtr;
	else
		up->sticky[Mctl] &= ~Dtr;
	uartwrreg(up, Mctl, 0);
}

/*
 *  toggle RTS
 */
void
uartrts(Uart *up, int n)
{
	if(n)
		up->sticky[Mctl] |= Rts;
	else
		up->sticky[Mctl] &= ~Rts;
	uartwrreg(up, Mctl, 0);
}

/*
 *  send break
 */
void
uartbreak(Uart *up, int ms)
{
	uartwrreg(up, Format, Break);
	tsleep(&u->p->sleep, return0, 0, ms);
	uartwrreg(up, Format, 0);
}

/*
 *  default is 9600 baud, 1 stop bit, 8 bit chars, no interrupts,
 *  transmit and receive enabled, interrupts disabled.
 */
void
uartsetup(void)
{
	Uart *up;
	static int already;

	if(already)
		return;
	already = 1;

	/*
	 *  get port addresses
	 */
	uart[0].port = 0x3F8;
	uart[1].port = 0x2F8;
	setvec(Uart0vec, uartintr0);
	setvec(Uart1vec, uartintr1);

	for(up = uart; up < &uart[2]; up++){
		memset(up->sticky, 0, sizeof(up->sticky));

		/*
		 *  set rate to 9600 baud.
		 *  8 bits/character.
		 *  1 stop bit.
		 */
		uartsetbaud(up, 9600);
		up->sticky[Format] = Bits8;
		uartwrreg(up, Format, 0);
	}
}

/*
 *  Queue n characters for output; if queue is full, we lose characters.
 *  Get the output going if it isn't already.
 */
void
uartputs(IOQ *cq, char *s, int n)
{
	Uart *up = cq->ptr;
	int st, ch, x;

	x = splhi();
	lock(cq);
	puts(cq, s, n);
	if(up->printing == 0){
		ch = getc(cq);
print("<start %2.2ux>", ch);/**/
		if(ch >= 0){
			up->printing = 1;
			while(uartrdreg(up, Lstat) & Outbusy)
				;
			outb(up->port + Data, ch);
		}
	}
	unlock(cq);
	splx(x);
}

/*
 *  a uart interrupt (a damn lot of work for one character)
 */
void
uartintr(Uart *up)
{
	int s;
	int ch;
	IOQ *cq;

	switch(uartrdreg(up, Istat)){
	case 3:
		/*
		 *  get any input characters
		 */
		cq = up->iq;
		while(uartrdreg(up, Lstat) & Inready){
			ch = uartrdreg(up, Data) & 0xff;
			if(cq->putc)
				(*cq->putc)(cq, ch);
			else {
				putc(cq, ch);
				if(up->delim[ch/8] & (1<<(ch&7)) )
					wakeup(&cq->r);
			}
		}
		break;
	case 5:
		/*
		 *  send next output character
		 */
		if((s & Outbusy)==0){
			cq = up->oq;
			lock(cq);
			ch = getc(cq);
			if(ch < 0){
				up->printing = 0;
				wakeup(&cq->r);
			}else
				outb(up->port + Data, ch);
			unlock(cq);
		}
		break;
	}
}
void
uartintr0(Ureg *ur)
{
	uartintr(&uart[0]);
}
void
uartintr1(Ureg *ur)
{
	uartintr(&uart[1]);
}

/*
 *  turn on a port's interrupts.  set DTR and RTS
 */
void
uartenable(Uart *up)
{
	/*
	 *  set up i/o routines
	 */
	if(up->oq){
		up->oq->puts = uartputs;
		up->oq->ptr = up;
		up->sticky[Iena] |= Ixmt;
	}
	if(up->iq){
		up->iq->ptr = up;
		up->sticky[Iena] |= Ircv;
	}

	/*
 	 *  turn on interrupts
	 */
	uartwrreg(up, Iena, 0);

	/*
	 *  turn on DTR and RTS
	 */
	uartdtr(up, 1);
	uartrts(up, 1);
}

/*
 *  set up an uart port as something other than a stream
 */
void
uartspecial(int port, IOQ *oq, IOQ *iq, int baud)
{
	Uart *up = &uart[port];

	uartsetup();
	up->nostream = 1;
	up->oq = oq;
	up->iq = iq;
	uartenable(up);
	uartsetbaud(up, baud);

	if(iq){
		/*
		 *  Stupid HACK to undo a stupid hack
		 */ 
		if(iq == &kbdq)
			kbdq.putc = kbdcr2nl;
	}
}

static void	uarttimer(Alarm*);
static int	uartputc(IOQ *, int);
static void	uartstopen(Queue*, Stream*);
static void	uartstclose(Queue*);
static void	uartoput(Queue*, Block*);
static void	uartkproc(void *);
Qinfo uartinfo =
{
	nullput,
	uartoput,
	uartstopen,
	uartstclose,
	"uart"
};

/*
 *  create a helper process per port
 */
static void
uarttimer(Alarm *a)
{
	Uart *up = a->arg;

	cancel(a);
	up->a = 0;
	wakeup(&up->iq->r);
}

static int
uartputc(IOQ *cq, int ch)
{
	Uart *up = cq->ptr; int r;

	r = putc(cq, ch);

	/*
	 *  pass upstream within up->delay milliseconds
	 */
	if(up->a==0){
		if(up->delay == 0)
			wakeup(&cq->r);
		else
			up->a = alarm(up->delay, uarttimer, up);
	}
	return r;
}

static void
uartstopen(Queue *q, Stream *s)
{
	Uart *up;
	char name[NAMELEN];

	kprint("uartstopen: q=0x%ux, inuse=%d, type=%d, dev=%d, id=%d\n",
		q, s->inuse, s->type, s->dev, s->id);
	up = &uart[s->id];
	qlock(up);
	up->wq = WR(q);
	WR(q)->ptr = up;
	RD(q)->ptr = up;
	up->delay = 64;
	up->iq->putc = uartputc;
	qunlock(up);

	/* start with all characters as delimiters */
	memset(up->delim, 1, sizeof(up->delim));
	
	if(up->kstarted == 0){
		up->kstarted = 1;
		sprint(name, "uart%d", s->id);
		kproc(name, uartkproc, up);
	}
}

static void
uartstclose(Queue *q)
{
	Uart *up = q->ptr;

	qlock(up);
	kprint("uartstclose: q=0x%ux, id=%d\n", q, up-uart);
	up->wq = 0;
	up->iq->putc = 0;
	WR(q)->ptr = 0;
	RD(q)->ptr = 0;
	qunlock(up);
}

static void
uartoput(Queue *q, Block *bp)
{
	Uart *up = q->ptr;
	IOQ *cq;
	int n, m;

	if(up == 0){
		freeb(bp);
		return;
	}
	cq = up->oq;
	if(waserror()){
		freeb(bp);
		nexterror();
	}
	if(bp->type == M_CTL){
		while (cangetc(cq))	/* let output drain */
			sleep(&cq->r, cangetc, cq);
		n = strtoul((char *)(bp->rptr+1), 0, 0);
		switch(*bp->rptr){
		case 'B':
		case 'b':
			uartsetbaud(up, n);
			break;
		case 'D':
		case 'd':
			uartdtr(up, n);
			break;
		case 'K':
		case 'k':
			uartbreak(up, n);
			break;
		case 'R':
		case 'r':
			uartrts(up, n);
			break;
		case 'W':
		case 'w':
			if(n>=0 && n<1000)
				up->delay = n;
			break;
		}
	}else while((m = BLEN(bp)) > 0){
		while ((n = canputc(cq)) == 0){
print("uartoput: sleeping\n");
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
uartkproc(void *a)
{
	Uart *up = a;
	IOQ *cq = up->iq;
	Block *bp;
	int n;

loop:
	while ((n = cangetc(cq)) == 0){
print("uart0 sleeping/n");
		sleep(&cq->r, cangetc, cq);
	}
	qlock(up);
	if(up->wq == 0){
		cq->out = cq->in;
	}else{
		bp = allocb(n);
		bp->flags |= S_DELIM;
		bp->wptr += gets(cq, bp->wptr, n);
		PUTNEXT(RD(up->wq), bp);
	}
	qunlock(up);
	goto loop;
}

enum{
	Qdir=		0,
	Qtty0=		STREAMQID(0, Sdataqid),
	Qtty0ctl=	STREAMQID(0, Sctlqid),
	Qtty1=		STREAMQID(1, Sdataqid),
	Qtty1ctl=	STREAMQID(1, Sctlqid),
};

Dirtab uartdir[]={
	"tty0",		{Qtty0},	0,		0666,
	"tty0ctl",	{Qtty0ctl},	0,		0666,
	"tty1",		{Qtty1},	0,		0666,
	"tty1ctl",	{Qtty1ctl},	0,		0666,
};

#define	NUart	(sizeof uartdir/sizeof(Dirtab))

/*
 *  allocate the queues if no one else has
 */
void
uartreset(void)
{
	Uart *up;

	uartsetup();
	for(up = uart; up < &uart[2]; up++){
		if(up->nostream)
			continue;
		up->iq = ialloc(sizeof(IOQ), 0);
		initq(up->iq);
		up->oq = ialloc(sizeof(IOQ), 0);
		initq(up->oq);
		uartenable(up);
	}
}

void
uartinit(void)
{
}

Chan*
uartattach(char *upec)
{
	return devattach('t', upec);
}

Chan*
uartclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
uartwalk(Chan *c, char *name)
{
	return devwalk(c, name, uartdir, NUart, devgen);
}

void
uartstat(Chan *c, char *dp)
{
	switch(c->qid.path){
	case Qtty0:
		streamstat(c, dp, "tty0");
		break;
	case Qtty1:
		streamstat(c, dp, "tty1");
		break;
	default:
		devstat(c, dp, uartdir, NUart, devgen);
		break;
	}
}

Chan*
uartopen(Chan *c, int omode)
{
	Uart *up;

	switch(c->qid.path){
	case Qtty0:
	case Qtty0ctl:
		up = &uart[0];
		break;
	case Qtty1:
	case Qtty1ctl:
		up = &uart[1];
		break;
	default:
		up = 0;
		break;
	}

	if(up && up->nostream)
		errors("in use");

	if((c->qid.path & CHDIR) == 0)
		streamopen(c, &uartinfo);
	return devopen(c, omode, uartdir, NUart, devgen);
}

void
uartcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
uartclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long
uartread(Chan *c, void *buf, long n, ulong offset)
{
	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, uartdir, NUart, devgen);
	case Qtty1ctl:
	case Qtty0ctl:
		return 0;
	}
	return streamread(c, buf, n);
}

long
uartwrite(Chan *c, void *va, long n, ulong offset)
{
	return streamwrite(c, va, n, 0);
}

void
uartremove(Chan *c)
{
	error(Eperm);
}

void
uartwstat(Chan *c, char *dp)
{
	error(Eperm);
}
