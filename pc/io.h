/*
 *  programmable interrupt vectors (for the 8259's)
 */
enum
{
	Bptvec=		3,		/* breakpoints */
	Mathemuvec=	7,		/* math coprocessor emulation interrupt */
	Mathovervec=	9,		/* math coprocessor overrun interrupt */
	Matherr1vec=	16,		/* math coprocessor error interrupt */
	Faultvec=	14,		/* page fault */

	Int0vec=	24,		/* first 8259 */
	 Clockvec=	Int0vec+0,	/*  clock interrupts */
	 Kbdvec=	Int0vec+1,	/*  keyboard interrupts */
	 Uart1vec=	Int0vec+3,	/*  modem line */
	 Uart0vec=	Int0vec+4,	/*  serial line */
	 Floppyvec=	Int0vec+6,	/*  floppy interrupts */
	 Parallelvec=	Int0vec+7,	/*  parallel port interrupts */
	Int1vec=	Int0vec+8,
	 Ethervec=	Int0vec+10,	/*  ethernet interrupt */
	 Mousevec=	Int0vec+12,	/*  mouse interrupt */
	 Matherr2vec=	Int0vec+13,	/*  math coprocessor */
	 Hardvec=	Int0vec+14,	/*  hard disk */

	Syscallvec=	64,
};

typedef struct EtherHw EtherHw;
typedef struct EtherBuf EtherBuf;
typedef struct EtherType EtherType;
typedef struct EtherCtlr EtherCtlr;

struct EtherHw {
	int	(*reset)(EtherCtlr*);
	void	(*init)(EtherCtlr*);
	void	(*mode)(EtherCtlr*, int);
	void	(*online)(EtherCtlr*, int);
	void	(*receive)(EtherCtlr*);
	void	(*transmit)(EtherCtlr*);
	void	(*intr)(EtherCtlr*);
	void	(*tweak)(EtherCtlr*);
	int	addr;			/* interface address */
	uchar	*ram;			/* interface shared memory address */
	int	bt16;			/* true if a 16 bit interface */
	int	irq;			/* interrupt level */
	int	size;
	uchar	tstart;
	uchar	pstart;
	uchar	pstop;
};

struct EtherBuf {
	uchar	owner;
	uchar	busy;
	ushort	len;
	uchar	pkt[sizeof(Etherpkt)];
};

enum {
	Host		= 0,		/* buffer owned by host */
	Interface	= 1,		/* buffer owned by interface */

	NType		= 9,		/* types/interface */
};

/*
 * one per ethernet packet type
 */
struct EtherType {
	QLock;
	Netprot;			/* stat info */
	int	type;			/* ethernet type */
	int	prom;			/* promiscuous mode */
	Queue	*q;
	int	inuse;
	EtherCtlr *ctlr;
};

/*
 * per ethernet
 */
struct EtherCtlr {
	QLock;

	EtherHw	*hw;
	int	present;

	ushort	nrb;		/* number of software receive buffers */
	ushort	ntb;		/* number of software transmit buffers */
	EtherBuf *rb;		/* software receive buffers */
	EtherBuf *tb;		/* software transmit buffers */

	uchar	ea[6];		/* ethernet address */
	uchar	ba[6];		/* broadcast address */

	Rendez	rr;		/* rendezvous for a receive buffer */
	ushort	rh;		/* first receive buffer belonging to host */
	ushort	ri;		/* first receive buffer belonging to interface */	

	Rendez	tr;		/* rendezvous for a transmit buffer */
	QLock	tlock;		/* semaphore on th */
	ushort	th;		/* first transmit buffer belonging to host */	
	ushort	ti;		/* first transmit buffer belonging to interface */	

	EtherType type[NType];
	uchar	prom;		/* true if promiscuous mode */
	uchar	kproc;		/* true if kproc started */
	char	name[NAMELEN];	/* name of kproc */
	Network	net;

	Queue	lbq;		/* software loopback packet queue */

	int	inpackets;
	int	outpackets;
	int	crcs;		/* input crc errors */
	int	oerrs;		/* output errors */
	int	frames;		/* framing errors */
	int	overflows;	/* packet overflows */
	int	buffs;		/* buffering errors */
};
