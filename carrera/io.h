/*
 * These register addresses have already been adjusted for BE operation
 *
 * The EISA memory address is completely fictional. Look at mmu.c for
 * a translation of Eisamphys into a 33 bit address.
 */
enum
{
	Devicephys	= 0x80000000,
	Eisaphys	= 0x90000000,	/* IO port physical */
	Devicevirt	= 0xE0000000,
	Dmabase		= 0xE0000000,	
	  R4030Isr	= 0xE0000204,	/* Interrupt status register */
	  R4030Et	= 0xE000020C,	/* Eisa error type */
	  R4030Rfa	= 0xE000003C,	/* Remote failed address */
	  R4030Mfa	= 0xE0000044,	/* Memory failed address */
	Sonicbase	= 0xE0001000,    	
	Scsibase 	= 0xE0002000,   	
	Floppybase	= 0xE0003000,	
	Clockbase	= 0xE0004000,	
	Uart0		= 0xE0006000,	
	Uart1		= 0xE0007000,	
	  UartFREQ	= 8000000,
	Centronics	= 0xE0008000,
	Nvram		= 0xE0009000, 	
	NvramRW		= 0xE000A000,
	NvramRO		= 0xE000B000,
	 Enetoffset	= 0,
	KeyboardIO	= 0xE0005000,
	 Keyctl		= 6,
	 Keydat		= 7,	
	MouseIO		= 0xE0005000,  	
	SoundIO		= 0xE000C000,
	EisaLatch	= 0xE000E007,
	Diag		= 0xE000F000,
	VideoCTL	= 0x60000000,	
	VideoMEM	= 0x40000000, 	
	I386ack		= 0xE000023f,
	EisaControl	= 0xE0010000,	/* Second 64K Page from Devicevirt */
	  Eisanmi	= EisaControl+0x77,
	Rtcindex	= Eisanmi,
	Rtcdata		= 0xE0004007,
	Eisamphys	= 0x91000000,
	  Eisavgaphys	= Eisamphys+0xA0000,
	Eisamvirt	= 0xE0300000,	/* Second 1M page entry from Intctl */
	Intctlphys	= 0xF0000000,	
	Intctlvirt	= 0xE0200000,
	  Uart1int	= (1<<9),
	  Uart0int	= (1<<8),
	  Mouseint	= (1<<7),
	  Keybint	= (1<<6),
	  Scsiint	= (1<<5),
	  Etherint	= (1<<4),
	  Videoint	= (1<<3),
	  Soundint	= (1<<2),
	  Floppyint	= (1<<1),
	  Parallelint	= (1<<0),
	Intenareg	= 0xE0200004,	/* Device interrupt enable */
	Intcause	= 0xE0200007,	/* Decice interrupt cause register */

	Ttbr		= 0x8000001C,	/* Translation table base address */
	Tlrb		= 0x80000024,	/* Translation table limit address */
	Tir		= 0x8000002c,	/* Translation invalidate register */
	Ntranslation	= 4096,		/* Number of translation registers */
};

typedef struct Tte Tte;
struct Tte
{
	ulong	hi;
	ulong	lo;
};

#define IO(type, reg)		(*(type*)reg)
#define QUAD(type, v)		(type*)(((ulong)(v)+7)&~7)
#define CACHELINE(type, v)	(type*)(((ulong)(v)+127)&~127)
#define UNCACHED(type, v)	(type*)((ulong)(v)|0xA0000000)

#define EISA(v)			(Eisamvirt+(v))
#define EISAINB(port)		(*(uchar*)((EisaControl+(port))^7))
#define EISAINW(port)		(*(ushort*)((EisaControl+(port))^6))
#define EISAOUTB(port, v)	EISAINB(port) = v
#define EISAOUTW(port, v)	EISAINW(port) = v
