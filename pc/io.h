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
	 PCMCIAvec=	Int0vec+5,	/*  PCMCIA card change */
	 Floppyvec=	Int0vec+6,	/*  floppy interrupts */
	 Parallelvec=	Int0vec+7,	/*  parallel port interrupts */
	Int1vec=	Int0vec+8,
	 Vector9=	Int0vec+9,	/*  unassigned */
	 Vector10=	Int0vec+10,	/*  unassigned, usually ethernet */
	 Vector11=	Int0vec+11,	/*  unassigned, usually scsi */
	 Mousevec=	Int0vec+12,	/*  mouse interrupt */
	 Matherr2vec=	Int0vec+13,	/*  math coprocessor */
	 Hardvec=	Int0vec+14,	/*  hard disk */
	 Vector15=	Int0vec+15,	/*  unassigned */

	Syscallvec=	64,
};

enum {
	MaxEISA		= 16,
	EISAconfig	= 0xC80,
};

/*
 * PCI Local Bus support.
 * Quick hack until we figure out how to
 * deal with EISA, PCI, PCMCIA, PnP, etc.
 */
enum {					/* configuration mechanism #1 */
	PCIaddr		= 0xCF8,	/* CONFIG_ADDRESS */
	PCIdata		= 0xCFC,	/* CONFIG_DATA */

					/* configuration mechanism #2 */
	PCIcse		= 0xCF8,	/* configuration space enable */
	PCIforward	= 0xCFA,	/* which bus */
	
	MaxPCI		= 32,		/* 16 for mechanism #2 */
};

typedef struct PCIcfg {
	ushort	vid;			/* vendor ID */
	ushort	did;			/* device ID */
	ushort	command;	
	ushort	status;	
	uchar	rid;			/* revision ID */
	uchar	loclass;		/* specific register-level programming interface */
	uchar	subclass;	
	uchar	baseclass;	
	uchar	clsize;			/* cache line size */
	uchar	latency;		/* latency timer */
	uchar	header;			/* header type */
	uchar	bist;			/* built-in self-test */
	ulong	baseaddr[6];		/* memory or I/O base address registers */
	ulong	reserved28[2];	
	ulong	romaddr;		/* expansion ROM base address */
	ulong	reserved34[2];	
	uchar	irq;			/* interrupt line */
	uchar	irp;			/* interrupt pin */
	uchar	mingnt;			/* burst period length */
	uchar	maxlat;			/* maximum latency between bursts */
} PCIcfg;
