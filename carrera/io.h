/*
 * These register addresses have already been adjusted for BE operation
 */
enum
{
	Devicephys	= 0x80000000,
	Devicevirt	= 0xE0000000,
	Dmabase		= 0xE0000000,	
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
	KeyboardIO	= 0xE0005000,	
	MouseIO		= 0xE0005000,  	
	SoundIO		= 0xE000C000,
	EisaLatch	= 0xE000E000,
	Diag		= 0xE000F000,
	VideoCTL	= 0x60000000,	
	VideoMEM	= 0x40000000, 	
	I386ack		= 0xE000023f,
	Intctlphys	= 0xF0000000,	
	Intctlvirt	= 0xE0040000,
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
	Intenareg	= 0xE0040004,	/* Device interrupt enable */
	Intcause	= 0xE0040007,	/* Decice interrupt cause register */

	Ttbr		= 0x8000001C,	/* Translation table base address */
	Tlrb		= 0x80000024,	/* Translation table limit address */
	Tir		= 0x8000002c,	/* Translation invalidate register */
	Ntranslation	= 4096,		/* Number of translation registers */
};

#define IO(type, reg)	(*(type*)reg)

typedef struct Tte Tte;
struct Tte
{
	ulong	hi;
	ulong	lo;
};
