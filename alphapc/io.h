enum {
	Uart0		= 0x3F8,
	Uart1		= 0x2F8,
	  UartFREQ	= 1843200,
};

enum {
/*	VectorPIC	= 32,		/* external i8259 interrupts */
	IrqCLOCK	= 0,
	IrqKBD		= 1,
	IrqUART1	= 3,
	IrqUART0	= 4,
	IrqPCMCIA	= 5,
	IrqFLOPPY	= 6,
	IrqLPT		= 7,
	IrqIRQ7		= 7,
	IrqAUX		= 12,		/* PS/2 port */
	IrqIRQ13	= 13,		/* coprocessor on 386 */
	IrqATA0		= 14,
	MaxIrqPIC	= 15,

/* deprecated: */
	VectorPIC	= 128,		/* external [A]PIC interrupts */
	VectorCLOCK	= VectorPIC+0,
	VectorKBD	= VectorPIC+1,
	VectorUART1	= VectorPIC+3,
	VectorUART0	= VectorPIC+4,
	VectorPCMCIA	= VectorPIC+5,
	VectorFLOPPY	= VectorPIC+6,
	VectorLPT	= VectorPIC+7,
	VectorIRQ7	= VectorPIC+7,
	VectorAUX	= VectorPIC+12,	/* PS/2 port */
	VectorIRQ13	= VectorPIC+13,	/* corocessor on x386 */
	VectorATA0	= VectorPIC+14,
	MaxVectorPIC	= VectorPIC+15,
	VectorPCI	= 16,		/* PCI bus (PLD) */
};

typedef struct Irq {
	void	(*f)(Ureg*, void*);	/* handler to call */
	void*	a;			/* argument to call it with */

	Irq*	next;			/* link to next handler */
} Irq;

typedef struct Irqctl {
	int	(*isr)(int);		/* get isr bit for this irq */
	int	(*eoi)(int);		/* eoi */
	int	isintr;

	Irq*	irq;			/* handlers on this IRQ */
} Irqctl;

enum {
	BusCBUS		= 0,		/* Corollary CBUS */
	BusCBUSII,			/* Corollary CBUS II */
	BusEISA,			/* Extended ISA */
	BusFUTURE,			/* IEEE Futurebus */
	BusINTERN,			/* Internal bus */
	BusISA,				/* Industry Standard Architecture */
	BusMBI,				/* Multibus I */
	BusMBII,			/* Multibus II */
	BusMCA,				/* Micro Channel Architecture */
	BusMPI,				/* MPI */
	BusMPSA,			/* MPSA */
	BusNUBUS,			/* Apple Macintosh NuBus */
	BusPCI,				/* Peripheral Component Interconnect */
	BusPCMCIA,			/* PC Memory Card International Association */
	BusTC,				/* DEC TurboChannel */
	BusVL,				/* VESA Local bus */
	BusVME,				/* VMEbus */
	BusXPRESS,			/* Express System Bus */
};

#define MKBUS(t,b,d,f)	(((t)<<24)|(((b)&0xFF)<<16)|(((d)&0x1F)<<11)|(((f)&0x07)<<8))
#define BUSFNO(tbdf)	(((tbdf)>>8)&0x07)
#define BUSDNO(tbdf)	(((tbdf)>>11)&0x1F)
#define BUSBNO(tbdf)	(((tbdf)>>16)&0xFF)
#define BUSTYPE(tbdf)	((tbdf)>>24)
#define BUSDF(tbdf)		((tbdf)&0x000FF00)
#define BUSBDF(tbdf)	((tbdf)&0x0FFFF00)
#define BUSUNKNOWN	(-1)

enum {
	MaxEISA		= 16,
	EISAconfig	= 0xC80,
};

/*
 * PCI support code.
 */
enum {					/* type 0 and type 1 pre-defined header */
	PciVID		= 0x00,		/* vendor ID */
	PciDID		= 0x02,		/* device ID */
	PciPCR		= 0x04,		/* command */
	PciPSR		= 0x06,		/* status */
	PciRID		= 0x08,		/* revision ID */
	PciCCRp		= 0x09,		/* programming interface class code */
	PciCCRu		= 0x0A,		/* sub-class code */
	PciCCRb		= 0x0B,		/* base class code */
	PciCLS		= 0x0C,		/* cache line size */
	PciLTR		= 0x0D,		/* latency timer */
	PciHDT		= 0x0E,		/* header type */
	PciBST		= 0x0F,		/* BIST */

	PciBAR0		= 0x10,		/* base address */
	PciBAR1		= 0x14,

	PciINTL		= 0x3C,		/* interrupt line */
	PciINTP		= 0x3D,		/* interrupt pin */
};

enum {					/* type 0 pre-defined header */
	PciBAR2		= 0x18,
	PciBAR3		= 0x1C,
	PciBAR4		= 0x20,
	PciBAR5		= 0x24,
	PciCIS		= 0x28,		/* cardbus CIS pointer */
	PciSVID		= 0x2C,		/* subsystem vendor ID */
	PciSID		= 0x2E,		/* cardbus CIS pointer */
	PciEBAR0	= 0x30,		/* xpansion ROM base address */
	PciMGNT		= 0x3E,		/* burst period length */
	PciMLT		= 0x3F,		/* maximum latency between bursts */
};

enum {					/* type 1 pre-defined header */
	PciPBN		= 0x18,		/* primary bus number */
	PciSBN		= 0x19,		/* secondary bus number */
	PciUBN		= 0x1A,		/* subordinate bus number */
	PciSLTR		= 0x1B,		/* secondary latency timer */
	PciIBR		= 0x1C,		/* I/O base */
	PciILR		= 0x1D,		/* I/O limit */
	PciSPSR		= 0x1E,		/* secondary status */
	PciMBR		= 0x20,		/* memory base */
	PciMLR		= 0x22,		/* memory limit */
	PciPMBR		= 0x24,		/* prefetchable memory base */
	PciPMLR		= 0x26,		/* prefetchable memory limit */
	PciPUBR		= 0x28,		/* prefetchable base upper 32 bits */
	PciPULR		= 0x2C,		/* prefetchable limit upper 32 bits */
	PciIUBR		= 0x30,		/* I/O base upper 16 bits */
	PciIULR		= 0x32,		/* I/O limit upper 16 bits */
	PciEBAR1	= 0x28,		/* expansion ROM base address */
	PciBCR		= 0x3E,		/* bridge control register */
};

typedef struct Pcidev Pcidev;
typedef struct Pcidev {
	int	tbdf;		/* type+bus+device+function */
	ushort	vid;			/* vendor ID */
	ushort	did;			/* device ID */

	struct {
		ulong	bar;		/* base address */
		int	size;
	} mem[3];

	uchar	intl;			/* interrupt line */
	ushort	ccru;


	Pcidev*	list;
	Pcidev*	bridge;			/* down a bus */
	Pcidev*	link;			/* next device on this bno */
};

/*
 * PCMCIA support code.
 */
/*
 * Map between ISA memory space and PCMCIA card memory space.
 */
struct PCMmap {
	ulong	ca;			/* card address */
	ulong	cea;			/* card end address */
	ulong	isa;			/* ISA address */
	int	len;			/* length of the ISA area */
	int	attr;			/* attribute memory */
	int	ref;
};

/*
 *  SCSI bus
 */
enum {
	MaxScsi		= 8,
	NTarget		= 8,		/* should be 16... */
};
struct Target {
	int	ctlrno;
	int	target;
	uchar*	inq;
	uchar*	scratch;

	Rendez	rendez;

	int	ok;
};

typedef int (*Scsiio)(Target*, int, uchar*, int, void*, int*);

typedef struct SCSIdev {
	char*	type;
	Scsiio	(*reset)(int, ISAConf*);
} SCSIdev;
