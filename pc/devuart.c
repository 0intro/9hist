#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"
#define		nelem(x)	(sizeof(x)/sizeof(x[0]))
/*
 *  Driver for an NS16450 serial port
 */
enum
{
	/*
	 *  register numbers
	 */
	Data=	0,		/* xmit/rcv buffer */
	Iena=	1,		/* interrdpt enable */
	 Ircv=	(1<<0),		/*  for char rcv'd */
	 Ixmt=	(1<<1),		/*  for xmit buffer empty */
	 Irstat=(1<<2),		/*  for change in rcv'er status */
	 Imstat=(1<<3),		/*  for change in modem status */
	Istat=	2,		/* interrdpt flag (read) */
	Tctl=	2,		/* test control (write) */
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
	 Inton=	(1<<3),		/*  turn on interrdpts */
	 Loop=	(1<<4),		/*  loop back */
	Lstat=	5,		/* line status */
	 Inready=(1<<0),	/*  receive buffer full */
	 Oerror=(1<<1),		/*  receiver overrun */
	 Perror=(1<<2),		/*  receiver parity error */
	 Ferror=(1<<3),		/*  rcv framing error */
	 Outready=(1<<5),	/*  output buffer full */
	Mstat=	6,		/* modem status */
	 Ctsc=	(1<<0),		/*  clear to send changed */
	 Dsrc=	(1<<1),		/*  data set ready changed */
	 Rire=	(1<<2),		/*  rising edge of ring indicator */
	 Dcdc=	(1<<3),		/*  data carrier detect changed */
	 Cts=	(1<<4),		/*  complement of clear to send line */
	 Dsr=	(1<<5),		/*  complement of data set ready line */
	 Ring=	(1<<6),		/*  complement of ring indicator line */
	 Dcd=	(1<<7),		/*  complement of data carrier detect line */
	Scratch=7,		/* scratchpad */
	Dlsb=	0,		/* divisor lsb */
	Dmsb=	1,		/* divisor msb */

	Serial=	0,
	Modem=	1,
};

typedef struct Uart	Uart;
struct Uart
{
	QLock;
	int	port;
	uchar	sticky[8];	/* sticky write register values */
	int	printing;	/* true if printing */
	int	enabled;
	int	cts;

	/* console interface */
	int	special;	/* can't use the stream interface */
	IOQ	*iq;		/* input character queue */
	IOQ	*oq;		/* output character queue */

	/* stream interface */
	Queue	*wq;		/* write queue */
	Rendez	r;		/* kproc waiting for input */
 	int	kstarted;	/* kproc started */

	/* error statistics */
	ulong	frame;
	ulong	overrun;
};

Uart	uart[34];
int	Nuart;		/* total no of uarts in the machine */
int	Nscard;		/* number of serial cards */
ISAConf	scard[5];	/* configs for the serial card */

#define UartFREQ 1843200

#define uartwrreg(u,r,v)	outb((u)->port + r, (u)->sticky[r] | (v))
#define uartrdreg(u,r)		inb((u)->port + r)

void	uartintr(Uart*);
void	uartintr0(Ureg*);
void	uartintr1(Ureg*);
void	uartintr2(Ureg*);
void	uartsetup(void);

/*
 *  set the baud rate by calculating and setting the baudrate
 *  generator constant.  This will work with fairly non-standard
 *  baud rates.
 */
void
uartsetbaud(Uart *dp, int rate)
{
	ulong brconst;

	brconst = (UartFREQ+8*rate-1)/(16*rate);

	uartwrreg(dp, Format, Dra);
	outb(dp->port+Dmsb, (brconst>>8) & 0xff);
	outb(dp->port+Dlsb, brconst & 0xff);
	uartwrreg(dp, Format, 0);
}

/*
 *  toggle DTR
 */
void
uartdtr(Uart *dp, int n)
{
	if(n)
		dp->sticky[Mctl] |= Dtr;
	else
		dp->sticky[Mctl] &= ~Dtr;
	uartwrreg(dp, Mctl, 0);
}

/*
 *  toggle RTS
 */
void
uartrts(Uart *dp, int n)
{
	if(n)
		dp->sticky[Mctl] |= Rts;
	else
		dp->sticky[Mctl] &= ~Rts;
	uartwrreg(dp, Mctl, 0);
}

/*
 *  send break
 */
void
uartbreak(Uart *dp, int ms)
{
	if(ms == 0)
		ms = 200;
	uartwrreg(dp, Format, Break);
	tsleep(&dp->sleep, return0, 0, ms);
	uartwrreg(dp, Format, 0);
}

/*
 *  set bits/char
 */
void
uartbits(Uart *dp, int bits)
{
	if(bits < 5 || bits > 8)
		error(Ebadarg);
	dp->sticky[Format] &= ~3;
	dp->sticky[Format] |= bits-5;
	uartwrreg(dp, Format, 0);
}

/*
 *  set parity
 */
void
uartparity(Uart *dp, int c)
{
	switch(c&0xff){
	case 'e':
		dp->sticky[Format] |= Pena|Peven;
		break;
	case 'o':
		dp->sticky[Format] &= ~Peven;
		dp->sticky[Format] |= Pena;
		break;
	default:
		dp->sticky[Format] &= ~(Pena|Peven);
		break;
	}
	uartwrreg(dp, Format, 0);
}

/*
 *  set stop bits
 */
void
uartstop(Uart *dp, int n)
{
	switch(n){
	case 1:
		dp->sticky[Format] &= ~Stop2;
		break;
	case 2:
	default:
		dp->sticky[Format] |= Stop2;
		break;
	}
	uartwrreg(dp, Format, 0);
}

/*
 *  modem flow control on/off (rts/cts)
 */
void
uartmflow(Uart *dp, int n)
{
	if(n){
		dp->sticky[Iena] |= Imstat;
		uartwrreg(dp, Iena, 0);
		dp->cts = uartrdreg(dp, Mstat) & Cts;
	} else {
		dp->sticky[Iena] &= ~Imstat;
		uartwrreg(dp, Iena, 0);
		dp->cts = 1;
	}
}


/*
 *  default is 9600 baud, 1 stop bit, 8 bit chars, no interrdpts,
 *  transmit and receive enabled, interrdpts disabled.
 */
void
uartsetup(void)
{
	Uart	*dp;
	int	i, j, baddr;
	static int already;

	if(already)
		return;
	already = 1;

	/*
	 *  set port addresses
	 */
	uart[0].port = 0x3F8;
	uart[1].port = 0x2F8;
	setvec(Uart0vec, uartintr0);
	setvec(Uart1vec, uartintr1);
	Nuart = 2;
	for(i = 0; isaconfig("serial", i, &scard[i]) && i < nelem(scard); i++) {
		/*
		 * mem gives base port address for uarts
		 * irq is interrdpt
		 * port is the polling port
		 * size is the number of serial ports on the same polling port
		 */
		setvec(Int0vec + scard[i].irq, uartintr2);
		baddr = scard[i].mem;
		for(j=0, dp = &uart[Nuart]; j < scard[i].size; baddr += 8, j++, dp++)
			dp->port = baddr;
		Nuart += scard[i].size;
	}
	Nscard = i;
	for(dp = uart; dp < &uart[Nuart]; dp++){
		memset(dp->sticky, 0, sizeof(dp->sticky));
		/*
		 *  set rate to 9600 baud.
		 *  8 bits/character.
		 *  1 stop bit.
		 *  interrdpts enabled.
		 */
		uartsetbaud(dp, 9600);
		dp->sticky[Format] = Bits8;
		uartwrreg(dp, Format, 0);
		dp->sticky[Mctl] |= Inton;
		uartwrreg(dp, Mctl, 0x0);
	}
}

/*
 *  Queue n characters for output; if queue is full, we lose characters.
 *  Get the output going if it isn't already.
 */
void
uartputs(IOQ *cq, char *s, int n)
{
	Uart *dp = cq->ptr;
	int ch, x, multiprocessor;
	int tries;

	multiprocessor = active.machs > 1;
	x = splhi();
	if(multiprocessor)
		lock(cq);
	puts(cq, s, n);
	if(dp->printing == 0){
		ch = getc(cq);
		if(ch >= 0){
			dp->printing = 1;
			for(tries = 0; tries<10000 && !(uartrdreg(dp, Lstat)&Outready);
				tries++)
				;
			outb(dp->port + Data, ch);
		}
	}
	if(multiprocessor)
		unlock(cq);
	splx(x);
}

/*
 *  a uart interrdpt (a damn lot of work for one character)
 */
void
uartintr(Uart *dp)
{
	int ch;
	IOQ *cq;
	int s, l, multiprocessor;

	multiprocessor = active.machs > 1;
	for(;;){
		s = uartrdreg(dp, Istat);
		switch(s){
		case 6:	/* receiver line status */
			l = uartrdreg(dp, Lstat);
			if(l & Ferror)
				dp->frame++;
			if(l & Oerror)
				dp->overrun++;
			break;
	
		case 4:	/* received data available */
			ch = uartrdreg(dp, Data) & 0xff;
			cq = dp->iq;
			if(cq == 0)
				break;
			if(cq->putc)
				(*cq->putc)(cq, ch);
			else
				putc(cq, ch);
			break;
	
		case 2:	/* transmitter empty */
			cq = dp->oq;
			if(cq == 0)
				break;
			if(multiprocessor)
				lock(cq);
			if(dp->cts == 0)
				dp->printing = 0;
			else {
				ch = getc(cq);
				if(ch < 0){
					dp->printing = 0;
					wakedp(&cq->r);
				}else
					outb(dp->port + Data, ch);
			}
			if(multiprocessor)
				unlock(cq);
			break;
	
		case 0:	/* modem status */
			ch = uartrdreg(dp, Mstat);
			if(ch & Ctsc){
				dp->cts = ch & Cts;
				cq = dp->oq;
				if(cq == 0)
					break;
				if(multiprocessor)
					lock(cq);
				if(dp->cts && dp->printing == 0){
					ch = getc(cq);
					if(ch >= 0){
						dp->printing = 1;
						outb(dp->port + Data, getc(cq));
					} else
						wakedp(&cq->r);
				}
				if(multiprocessor)
					unlock(cq);
			}
			break;
	
		default:
			if(s&1)
				return;
			print("weird modem interrdpt #%2.2ux\n", s);
			break;
		}
	}
}
void
uartintr0(Ureg *ur)
{
	USED(ur);
	uartintr(&uart[0]);
}
void
uartintr1(Ureg *ur)
{
	USED(ur);
	uartintr(&uart[1]);
}
void
uartintr2(Ureg *ur)
{
	uchar	i, j, nuart, n;

	USED(ur);
	for(nuart=2, j = 0; j < Nscard; nuart += scard[j].size, j++) {
		n = ~inb(scard[j].port);
		if(n == 0)
			continue;
		for(i = 0; n; i++){
			if(n & 1)
				uartintr(&uart[nuart + i]);
			n >>= 1;
		}
	}
}

void
uartclock(void)
{
	Uart *dp;
	IOQ *cq;

	for(dp = uart; dp < &uart[Nuart]; dp++){
		cq = dp->iq;
		if(dp->wq && cangetc(cq))
			wakedp(&cq->r);
	}
}


/*
 *  turn on a port's interrdpts.  set DTR and RTS
 */
void
uartenable(Uart *dp)
{
	/*
	 *  turn on power to the port
	 */
	if(dp == &uart[Modem]){
		if(modem(1) < 0)
			print("can't turn on modem speaker\n");
	} else {
		if(serial(1) < 0)
			print("can't turn on serial port power\n");
	}

	/*
	 *  set dp i/o routines
	 */
	if(dp->oq){
		dp->oq->puts = uartputs;
		dp->oq->ptr = dp;
	}
	if(dp->iq){
		dp->iq->ptr = dp;
	}
	dp->enabled = 1;

	/*
 	 *  turn on interrdpts
	 */
	dp->sticky[Iena] = Ircv | Ixmt | Irstat;
	uartwrreg(dp, Iena, 0);

	/*
	 *  turn on DTR and RTS
	 */
	uartdtr(dp, 1);
	uartrts(dp, 1);

	/*
	 *  assume we can send
	 */
	dp->cts = 1;
}

/*
 *  turn off the uart
 */
void
uartdisable(Uart *dp)
{

	/*
 	 *  turn off interrdpts
	 */
	dp->sticky[Iena] = 0;
	uartwrreg(dp, Iena, 0);

	/*
	 *  revert to default settings
	 */
	dp->sticky[Format] = Bits8;
	uartwrreg(dp, Format, 0);

	/*
	 *  turn off DTR and RTS
	 */
	uartdtr(dp, 0);
	uartrts(dp, 0);
	dp->enabled = 0;

	/*
	 *  turn off power
	 */
	if(dp == &uart[Modem]){
		if(modem(0) < 0)
			print("can't turn off modem speaker\n");
	} else {
		if(serial(0) < 0)
			print("can't turn off serial power\n");
	}
}

/*
 *  set dp an uart port as something other than a stream
 */
void
uartspecial(int port, IOQ *oq, IOQ *iq, int baud)
{
	Uart *dp = &uart[port];

	uartsetup();
	dp->special = 1;
	dp->oq = oq;
	dp->iq = iq;
	uartenable(dp);
	if(baud)
		uartsetbaud(dp, baud);

	if(iq){
		/*
		 *  Stdpid HACK to undo a stdpid hack
		 */ 
		if(iq == &kbdq)
			kbdq.putc = kbdcr2nl;
	}
}

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

static void
uartstopen(Queue *q, Stream *s)
{
	Uart *dp;
	char name[NAMELEN];

	dp = &uart[s->id];
	dp->iq->putc = 0;
	uartenable(dp);

	qlock(dp);
	dp->wq = WR(q);
	WR(q)->ptr = dp;
	RD(q)->ptr = dp;
	qunlock(dp);

	if(dp->kstarted == 0){
		dp->kstarted = 1;
		sprint(name, "uart%d", s->id);
		kproc(name, uartkproc, dp);
	}
}

static void
uartstclose(Queue *q)
{
	Uart *dp = q->ptr;

	if(dp->special)
		return;

	uartdisable(dp);

	qlock(dp);
	kprint("uartstclose: q=0x%ux, id=%d\n", q, dp-uart);
	dp->wq = 0;
	dp->iq->putc = 0;
	WR(q)->ptr = 0;
	RD(q)->ptr = 0;
	qunlock(dp);
}

static void
uartoput(Queue *q, Block *bp)
{
	Uart *dp = q->ptr;
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
		while (cangetc(cq))	/* let output drain */
			sleep(&cq->r, cangetc, cq);
		n = strtoul((char *)(bp->rptr+1), 0, 0);
		switch(*bp->rptr){
		case 'B':
		case 'b':
			if(n <= 0)
				error(Ebadctl);
			uartsetbaud(dp, n);
			break;
		case 'D':
		case 'd':
			uartdtr(dp, n);
			break;
		case 'K':
		case 'k':
			uartbreak(dp, n);
			break;
		case 'L':
		case 'l':
			uartbits(dp, n);
			break;
		case 'm':
		case 'M':
			uartmflow(dp, n);
			break;
		case 'P':
		case 'p':
			uartparity(dp, *(bp->rptr+1));
			break;
		case 'R':
		case 'r':
			uartrts(dp, n);
			break;
		case 'S':
		case 's':
			uartstop(dp, n);
			break;
		}
	}else while((m = BLEN(bp)) > 0){
		while ((n = canputc(cq)) == 0){
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
 *  process to send bytes dpstream for a port
 */
static void
uartkproc(void *a)
{
	Uart *dp = a;
	IOQ *cq = dp->iq;
	Block *bp;
	int n;
	ulong frame, overrun;
	static ulong ints;

	frame = 0;
	overrun = 0;

	if(waserror())
		print("uartkproc got an error\n");

	for(;;){
		sleep(&cq->r, cangetc, cq);
		if((ints++ & 0x1f) == 0)
			lights((ints>>5)&1);
		qlock(dp);
		if(dp->wq == 0){
			cq->out = cq->in;
		}else{
			n = cangetc(cq);
			bp = allocb(n);
			bp->flags |= S_DELIM;
			bp->wptr += gets(cq, bp->wptr, n);
			PUTNEXT(RD(dp->wq), bp);
		}
		qunlock(dp);
		if(dp->frame != frame){
			kprint("uart%d: %d framing\n", dp-uart, dp->frame);
			frame = dp->frame;
		}
		if(dp->overrun != overrun){
			kprint("uart%d: %d overruns\n", dp-uart, dp->overrun);
			overrun = dp->overrun;
		}
	}
}

enum{
	Qdir=		0,
};

Dirtab uartdir[2*nelem(uart)];

/*
 *  allocate the queues if no one else has
 */
void
uartreset(void)
{
	Uart *dp;

	uartsetup();
	for(dp = uart; dp < &uart[Nuart]; dp++){
		if(dp->special)
			continue;
		dp->iq = xalloc(sizeof(IOQ));
		initq(dp->iq);
		dp->oq = xalloc(sizeof(IOQ));
		initq(dp->oq);
	}
}

void
uartinit(void)
{
	int	i;

	for(i=0; i < 2*Nuart; ++i) {
		if(i & 1 != 0) {
			sprint(uartdir[i].name, "eia%dctl", i/2);
			uartdir[i].qid.path = STREAMQID(i/2, Sctlqid);
		} else {
			sprint(uartdir[i].name, "eia%d", i/2);
			uartdir[i].qid.path = STREAMQID(i/2, Sdataqid);
		}
		uartdir[i].length = 0;
		uartdir[i].perm = 0666;
	}
}

Chan*
uartattach(char *dpec)
{
	return devattach('t', dpec);
}

Chan*
uartclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
uartwalk(Chan *c, char *name)
{
	return devwalk(c, name, uartdir, Nuart * 2, devgen);
}

void
uartstat(Chan *c, char *dir)
{
	int	i;

	for(i=0; i < 2*Nuart; i += 2)
		if(c->qid.path == uartdir[i].qid.path) {
			streamstat(c, dir, uartdir[i].name, uartdir[i].perm);
			return;
		}
	devstat(c, dir, uartdir, Nuart * 2, devgen);
}

Chan*
uartopen(Chan *c, int omode)
{
	Uart *dp;
	int	i;

	dp = 0;
	for(i=0; i < 2*Nuart; ++i)
		if(c->qid.path == uartdir[i].qid.path) {
			dp = &uart[i/2];
			break;
		}

	if(dp && dp->special)
		error(Einuse);
	if((c->qid.path & CHDIR) == 0)
		streamopen(c, &uartinfo);
	return devopen(c, omode, uartdir, Nuart * 2, devgen);
}

void
uartcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
uartclose(Chan *c)
{
	if(c->stream)
		streamclose(c);
}

long
uartstatus(Uart *dp, void *buf, long n, ulong offset)
{
	uchar mstat;
	uchar tstat;
	char str[128];

	str[0] = 0;
	tstat = dp->sticky[Mctl];
	mstat = uartrdreg(dp, Mstat);
	if(mstat & Cts)
		strcat(str, " cts");
	if(mstat & Dsr)
		strcat(str, " dsr");
	if(mstat & Ring)
		strcat(str, " ring");
	if(mstat & Dcd)
		strcat(str, " dcd");
	if(tstat & Dtr)
		strcat(str, " dtr");
	if(tstat & Dtr)
		strcat(str, " rts");
	return readstr(offset, buf, n, str);
}

long
uartread(Chan *c, void *buf, long n, ulong offset)
{
	int i;
	long qpath;

	USED(offset);
	qpath = c->qid.path & ~CHDIR;
	if(qpath == Qdir)
		return devdirread(c, buf, n, uartdir, Nuart * 2, devgen);
	for(i=1; i < 2*Nuart; i += 2)
		if(qpath == uartdir[i].qid.path)
			return uartstatus(&uart[i/2], buf, n, offset);
	return streamread(c, buf, n);
}

long
uartwrite(Chan *c, void *va, long n, ulong offset)
{
	USED(offset);
	return streamwrite(c, va, n, 0);
}

void
uartremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
uartwstat(Chan *c, char *dir)
{
	USED(c, dir);
	error(Eperm);
}
