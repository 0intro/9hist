#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"errno.h"

#include	"devtab.h"

int	duartacr;
int	duartimr;
void	(*kprofp)(ulong);

/*
 * Register set for half the duart.  There are really two sets.
 */
struct Duart{
	uchar	mr1_2;		/* Mode Register Channels 1 & 2 */
	uchar	sr_csr;		/* Status Register/Clock Select Register */
	uchar	cmnd;		/* Command Register */
	uchar	data;		/* RX Holding / TX Holding Register */
	uchar	ipc_acr;	/* Input Port Change/Aux. Control Register */
#define	ivr	ivr		/* Interrupt Vector Register */
	uchar	is_imr;		/* Interrupt Status/Interrupt Mask Register */
#define	ip_opcr	is_imr		/* Input Port/Output Port Configuration Register */
	uchar	ctur;		/* Counter/Timer Upper Register */
#define	scc_sopbc ctur		/* Start Counter Command/Set Output Port Bits Command */
	uchar	ctlr;		/* Counter/Timer Lower Register */
#define	scc_ropbc ctlr		/* Stop Counter Command/Reset Output Port Bits Command */
};

enum{
	CHAR_ERR	=0x00,	/* MR1x - Mode Register 1 */
	PAR_ENB		=0x00,
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
	BD38400		=0xCC|0x0000,
	BD19200		=0xCC|0x0100,
	BD9600		=0xBB|0x0000,
	BD4800		=0x99|0x0000,
	BD2400		=0x88|0x0000,
	BD1200		=0x66|0x0000,
	BD300		=0x44|0x0000,
	IM_IPC		=0x80,	/* IMRx/ISRx - Interrupt Mask/Interrupt Status */
	IM_DBB		=0x40,
	IM_RRDYB	=0x20,
	IM_XRDYB	=0x10,
	IM_CRDY		=0x08,
	IM_DBA		=0x04,
	IM_RRDYA	=0x02,
	IM_XRDYA	=0x01,
};

/*
 *  software info for a serial duart interface
 */
typedef struct Duartport	Duartport;
struct Duartport
{
	QLock;
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
Duartport	duartport[1];

uchar keymap[]={
/*80*/	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x8e,	0x58,
/*90*/	0x90,	0x91,	0x92,	0x93,	0x94,	0x95,	0x96,	0x97,
	0x98,	0x99,	0x9a,	0x9b,	0x58,	0x58,	0x58,	0x58,
/*A0*/	0x58,	0xa1,	0xa2,	0xa3,	0xa4,	0xa5,	0xa6,	0xa7,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0xae,	0xaf,
/*B0*/	0xb0,	0xb1,	0xb2,	0xb3,	0xb4,	0xb5,	0xb6,	0xb7,
	0xb8,	0xb9,	0x00,	0xbb,	0x1e,	0xbd,	0x60,	0x1f,
/*C0*/	0xc0,	0xc1,	0xc2,	0xc3,	0xc4,	0x58,	0xc6,	0x0a,
	0xc8,	0xc9,	0xca,	0xcb,	0xcc,	0xcd,	0xce,	0xcf,
/*D0*/	0x09,	0x08,	0xd2,	0xd3,	0xd4,	0xd5,	0xd6,	0xd7,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x7f,	0x58,
/*E0*/	0x58,	0x58,	0xe2,	0x1b,	0x0d,	0xe5,	0x58,	0x0a,
	0xe8,	0xe9,	0xea,	0xeb,	0xec,	0xed,	0xee,	0xef,
/*F0*/	0x09,	0x08,	0xb2,	0x1b,	0x0d,	0xf5,	0x81,	0x58,
	0x58,	0x58,	0x58,	0x58,	0x58,	0x58,	0x7f,	0x80,
};

void
duartinit(void)
{
	Duart *duart;
	static int already;

	if(already)
		return;
	already = 1;

	duart  =  DUARTREG;

	/*
	 * Keyboard
	 */
	duart[0].cmnd = RESET_RCV|DIS_TX|DIS_RX;
	duart[0].cmnd = RESET_TRANS;
	duart[0].cmnd = RESET_ERR;
	duart[0].cmnd = RESET_MR;
	duart[0].mr1_2 = CHAR_ERR|PAR_ENB|EVEN_PAR|CBITS8;
	duart[0].mr1_2 = NORM_OP|ONESTOPB;
	duart[0].sr_csr = BD4800;

	/*
	 * RS232
	 */
	duart[1].cmnd = RESET_RCV|DIS_TX|DIS_RX;
	duart[1].cmnd = RESET_TRANS;
	duart[1].cmnd = RESET_ERR;
	duart[1].cmnd = RESET_MR;
	duart[1].mr1_2 = CHAR_ERR|NO_PAR|CBITS8;
	duart[1].mr1_2 = NORM_OP|ONESTOPB;
	duart[1].sr_csr = BD9600;

	/*
	 * Output port
	 */
	duart[0].ipc_acr = duartacr = 0xB7;	/* allow change	of state interrupt */
	duart[1].ip_opcr = 0x00;
	duart[1].scc_ropbc = 0xFF;	/* make sure the port is reset first */
	duart[1].scc_sopbc = 0x04;	/* dtr = 1, pp = 01 */
	duart[0].is_imr = duartimr = IM_IPC|IM_RRDYB|IM_XRDYB|IM_RRDYA|IM_XRDYA;
	duart[0].cmnd = ENB_TX|ENB_RX;	/* enable TX and RX last */
	duart[1].cmnd = ENB_TX|ENB_RX;

	/*
	 * Initialize keyboard
	 */
	while (!(duart[0].sr_csr & (XMT_EMT|XMT_RDY)))
		;
	duart[0].data = 0x02;
}


void
duartbaud(int b)
{
	int x = 0;
	Duart *duart = DUARTREG;

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
		error(Ebadarg);
	}
	if(x & 0x0100)
		duart[0].ipc_acr = duartacr |= 0x80;
	else
		duart[0].ipc_acr = duartacr &= ~0x80;
	duart[1].sr_csr = x;
}

void
duartdtr(int val)
{
	Duart *duart = DUARTREG;
	if (val)
		duart[1].scc_ropbc=0x01;
	else
		duart[1].scc_sopbc=0x01;
}

void
duartbreak(int ms)
{
	static QLock brk;
	Duart *duart = DUARTREG;
	if (ms<=0 || ms >20000)
		error(Ebadarg);
	qlock(&brk);
	duart[0].is_imr = duartimr &= ~IM_XRDYB;
	duart[1].cmnd = STRT_BRK|ENB_TX;
	tsleep(&u->p->sleep, return0, 0, ms);
	duart[1].cmnd = STOP_BRK|ENB_TX;
	duart[0].is_imr = duartimr |= IM_XRDYB;
	qunlock(&brk);
}

enum{
	Kptime=200
};
void
duartstarttimer(void)
{
	Duart *duart;
	char x;

	duart = DUARTREG;
	duart[0].ctur = (Kptime)>>8;
	duart[0].ctlr = (Kptime)&255;
	duart[0].is_imr = duartimr |= IM_CRDY;
	x = duart[1].scc_sopbc;
	USED(x);
}

void
duartstoptimer(void)
{
	Duart *duart;
	char x;

	duart = DUARTREG;
	x = duart[1].scc_ropbc;
	USED(x);
	duart[0].is_imr = duartimr &= ~IM_CRDY;
}

struct latin
{
	uchar	l;
	char	c[2];
}latintab[] = {
	'',	"!!",	/* spanish initial ! */
	'',	"c|",	/* cent */
	'',	"c$",	/* cent */
	'',	"l$",	/* pound sterling */
	'',	"g$",	/* general currency */
	'',	"y$",	/* yen */
	'',	"j$",	/* yen */
	'',	"||",	/* broken vertical bar */
	'',	"SS",	/* section symbol */
	'',	"\"\"",	/* dieresis */
	'',	"cr",	/* copyright */
	'',	"cO",	/* copyright */
	'',	"sa",	/* super a, feminine ordinal */
	'',	"<<",	/* left angle quotation */
	'',	"no",	/* not sign, hooked overbar */
	'',	"--",	/* soft hyphen */
	'',	"rg",	/* registered trademark */
	'',	"__",	/* macron */
	'',	"s0",	/* degree (sup o) */
	'',	"+-",	/* plus-minus */
	'',	"s2",	/* sup 2 */
	'',	"s3",	/* sup 3 */
	'',	"''",	/* grave accent */
	'',	"mu",	/* mu */
	'',	"pg",	/* paragraph (pilcrow) */
	'',	"..",	/* centered . */
	'',	",,",	/* cedilla */
	'',	"s1",	/* sup 1 */
	'',	"so",	/* sup o */
	'',	">>",	/* right angle quotation */
	'',	"14",	/* 1/4 */
	'',	"12",	/* 1/2 */
	'',	"34",	/* 3/4 */
	'',	"??",	/* spanish initial ? */
	'',	"A`",	/* A grave */
	'',	"A'",	/* A acute */
	'',	"A^",	/* A circumflex */
	'',	"A~",	/* A tilde */
	'',	"A\"",	/* A dieresis */
	'',	"A:",	/* A dieresis */
	'',	"Ao",	/* A circle */
	'',	"AO",	/* A circle */
	'',	"Ae",	/* AE ligature */
	'',	"AE",	/* AE ligature */
	'',	"C,",	/* C cedilla */
	'',	"E`",	/* E grave */
	'',	"E'",	/* E acute */
	'',	"E^",	/* E circumflex */
	'',	"E\"",	/* E dieresis */
	'',	"E:",	/* E dieresis */
	'',	"I`",	/* I grave */
	'',	"I'",	/* I acute */
	'',	"I^",	/* I circumflex */
	'',	"I\"",	/* I dieresis */
	'',	"I:",	/* I dieresis */
	'',	"D-",	/* Eth */
	'',	"N~",	/* N tilde */
	'',	"O`",	/* O grave */
	'',	"O'",	/* O acute */
	'',	"O^",	/* O circumflex */
	'',	"O~",	/* O tilde */
	'',	"O\"",	/* O dieresis */
	'',	"O:",	/* O dieresis */
	'',	"OE",	/* O dieresis */
	'',	"Oe",	/* O dieresis */
	'',	"xx",	/* times sign */
	'',	"O/",	/* O slash */
	'',	"U`",	/* U grave */
	'',	"U'",	/* U acute */
	'',	"U^",	/* U circumflex */
	'',	"U\"",	/* U dieresis */
	'',	"U:",	/* U dieresis */
	'',	"UE",	/* U dieresis */
	'',	"Ue",	/* U dieresis */
	'',	"Y'",	/* Y acute */
	'',	"P|",	/* Thorn */
	'',	"Th",	/* Thorn */
	'',	"TH",	/* Thorn */
	'',	"ss",	/* shadp s */
	'',	"a`",	/* a grave */
	'',	"a'",	/* a acute */
	'',	"a^",	/* a circumflex */
	'',	"a~",	/* a tilde */
	'',	"a\"",	/* a dieresis */
	'',	"a:",	/* a dieresis */
	'',	"ao",	/* a circle */
	'',	"ae",	/* ae ligature */
	'',	"c,",	/* c cedilla */
	'',	"e`",	/* e grave */
	'',	"e'",	/* e acute */
	'',	"e^",	/* e circumflex */
	'',	"e\"",	/* e dieresis */
	'',	"e:",	/* e dieresis */
	'',	"i`",	/* i grave */
	'',	"i'",	/* i acute */
	'',	"i^",	/* i circumflex */
	'',	"i\"",	/* i dieresis */
	'',	"i:",	/* i dieresis */
	'',	"d-",	/* eth */
	'',	"n~",	/* n tilde */
	'',	"o`",	/* o grave */
	'',	"o'",	/* o acute */
	'',	"o^",	/* o circumflex */
	'',	"o~",	/* o tilde */
	'',	"o\"",	/* o dieresis */
	'',	"o:",	/* o dieresis */
	'',	"oe",	/* o dieresis */
	'',	"-:",	/* divide sign */
	'',	"o/",	/* o slash */
	'',	"u`",	/* u grave */
	'',	"u'",	/* u acute */
	'',	"u^",	/* u circumflex */
	'',	"u\"",	/* u dieresis */
	'',	"u:",	/* u dieresis */
	'',	"ue",	/* u dieresis */
	'',	"y'",	/* y acute */
	'',	"th",	/* thorn */
	'',	"p|",	/* thorn */
	'',	"y\"",	/* y dieresis */
	'',	"y:",	/* y dieresis */
	0,	0,
};

int
latin1(int k1, int k2)
{
	int i;
	struct latin *l;

	for(l=latintab; l->l; l++)
		if(k1==l->c[0] && k2==l->c[1])
			return l->l;
	return 0;
}

/*
 *  a serial line input interrupt
 */
void
duartrintr(char ch)
{
	IOQ *cq;
	Duartport *dp = duartport;

	cq = dp->iq;
	if(cq->putc)
		(*cq->putc)(cq, ch);
	else {
		putc(cq, ch);
		if(dp->delim[ch/8] & (1<<(ch&7)) )
			wakeup(&cq->r);
	}
}

/*
 *  a serial line output interrupt
 */
void
duartxintr(void)
{
	int ch;
	IOQ *cq;
	Duartport *dp = duartport;
	Duart *duart;

	cq = dp->oq;
	lock(cq);
	ch = getc(cq);
	duart = DUARTREG;
	if(ch < 0){
		dp->printing = 0;
		wakeup(&cq->r);
		duart[1].cmnd = DIS_TX;
	} else
		duart[1].data = ch;
	unlock(cq);
}


void
duartintr(Ureg *ur)
{
	int cause, status, c;
	Duart *duart;
	static int kbdstate, k1, k2;

	duart = DUARTREG;
	cause = duart->is_imr;
	/*
	 * I can guess your interrupt.
	 */
	/*
	 * Is it 0?
	 */
	if(cause & IM_CRDY){
		if(kprofp)
			(*kprofp)(ur->pc);
		c = duart[1].scc_ropbc;
		USED(c);
		duart[0].ctur = (Kptime)>>8;
		duart[0].ctlr = (Kptime)&255;
		c = duart[1].scc_sopbc;
		USED(c);
		return;
	}
	/*
	 * Is it 1?
	 */
	if(cause & IM_RRDYA){		/* keyboard input */
		status = duart->sr_csr;
		c = duart->data;
		if(status & (FRM_ERR|OVR_ERR|PAR_ERR))
			duart->cmnd = RESET_ERR;
		if(status & PAR_ERR) /* control word: caps lock (0x4) or repeat (0x10) */
			kbdrepeat((c&0x10) == 0);
		else{
			if(c == 0x7F)	/* VIEW key (bizarre) */
				c = 0xFF;
			if(c == 0xB6)	/* NUM PAD */
				kbdstate = 1;
			else{
				if(c & 0x80)
					c = keymap[c&0x7F];
				switch(kbdstate){
				case 1:
					k1 = c;
					kbdstate = 2;
					break;
				case 2:
					k2 = c;
					c = latin1(k1, k2);
					if(c == 0){
						kbdputc(&kbdq, k1);
						c = k2;
					}
					/* fall through */
				default:
					kbdstate = 0;
					kbdputc(&kbdq, c);
				}
			}
		}
	}
	/*
	 * Is it 2?
	 */
	while(cause & IM_RRDYB){	/* duart input */
		status = duart[1].sr_csr;
		c = duart[1].data;
		if(status & (FRM_ERR|OVR_ERR|PAR_ERR))
			duart[1].cmnd = RESET_ERR;
		else
			duartrintr(c);
		cause = duart->is_imr;
	}
	/*
	 * Is it 3?
	 */
	if(cause & IM_XRDYB)		/* duart output */
		duartxintr();
	/*
	 * Is it 4?
	 */
	if(cause & IM_XRDYA)
		duart[0].cmnd = DIS_TX;
	/*
	 * Is it 5?
	 */
	if(cause & IM_IPC)
		mousebuttons((~duart[0].ipc_acr) & 7);
}


/*
 *  Queue n characters for output; if queue is full, we lose characters.
 *  Get the output going if it isn't already.
 */
void
duartputs(IOQ *cq, char *s, int n)
{
	int ch, x;
	Duartport *dp = duartport;
	Duart *duart;

	x = splduart();
	lock(cq);
	puts(cq, s, n);
	if(dp->printing == 0){
		ch = getc(cq);
		if(ch >= 0){
			dp->printing = 1;
			duart = DUARTREG;
			duart[1].cmnd = ENB_TX;
			while(!(duart[1].sr_csr & (XMT_RDY|XMT_EMT)))
				;
			duart[1].data = ch;
		}
	}
	unlock(cq);
	splx(x);
}

void
duartenable(Duartport *dp)
{
	/*
	 *  set up i/o routines
	 */
	if(dp->oq){
		dp->oq->puts = duartputs;
		dp->oq->ptr = dp;
	}
	if(dp->iq)
		dp->iq->ptr = dp;
}

/*
 *  set up an duart port as something other than a stream
 */
void
duartspecial(int port, IOQ *oq, IOQ *iq, int baud)
{
	Duartport *dp = &duartport[port];

	dp->nostream = 1;
	dp->oq = oq;
	dp->iq = iq;
	duartenable(dp);
	duartbaud(baud);
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

	if(s->id > 0)
		panic("duartstopen");
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
		while (cangetc(cq))	/* let output drain */
			sleep(&cq->r, cangetc, cq);
		n = strtoul((char *)(bp->rptr+1), 0, 0);
		switch(*bp->rptr){
		case 'B':
		case 'b':
			duartbaud(n);
			break;
		case 'D':
		case 'd':
			duartdtr(n);
			break;
		case 'K':
		case 'k':
			duartbreak(n);
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

enum{
	Qdir=		0,
	Qtty0=		STREAMQID(0, Sdataqid),
	Qtty0ctl=	STREAMQID(0, Sctlqid),
};

Dirtab duartdir[]={
	"tty0",		{Qtty0},	0,		0666,
	"tty0ctl",	{Qtty0ctl},	0,		0666,
};

#define	NDuartport	(sizeof duartdir/sizeof(Dirtab))

/*
 *  allocate the queues if no one else has
 */
void
duartreset(void)
{
	Duartport *dp = duartport;

	if(dp->nostream)
		return;
	dp->iq = ialloc(sizeof(IOQ), 0);
	initq(dp->iq);
	dp->oq = ialloc(sizeof(IOQ), 0);
	initq(dp->oq);
	duartenable(dp);
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
	return devwalk(c, name, duartdir, NDuartport, devgen);
}

void
duartstat(Chan *c, char *dp)
{
	switch(c->qid.path){
	case Qtty0:
		streamstat(c, dp, "tty0");
		break;
	default:
		devstat(c, dp, duartdir, NDuartport, devgen);
		break;
	}
}

Chan*
duartopen(Chan *c, int omode)
{
	Duartport *dp;

	switch(c->qid.path){
	case Qtty0:
	case Qtty0ctl:
		dp = &duartport[0];
		break;
	default:
		dp = 0;
		break;
	}

	if(dp && dp->nostream)
		errors("in use");

	if((c->qid.path & CHDIR) == 0)
		streamopen(c, &duartinfo);
	return devopen(c, omode, duartdir, NDuartport, devgen);
}

void
duartcreate(Chan *c, char *name, int omode, ulong perm)
{
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
	int s;
	Duart *duart = DUARTREG;

	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, duartdir, NDuartport, devgen);
	case Qtty0ctl:
		if(offset)
			return 0;
		s = splhi();
		*(uchar *)buf = duart[1].ip_opcr;
		splx(s);
		return 1;
	}
	return streamread(c, buf, n);
}

long
duartwrite(Chan *c, void *va, long n, ulong offset)
{
	return streamwrite(c, va, n, 0);
}

void
duartremove(Chan *c)
{
	error(Eperm);
}

void
duartwstat(Chan *c, char *dp)
{
	error(Eperm);
}
