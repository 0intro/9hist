/*
 *  Definitions for IO devices.  Used only in C.
 */
typedef struct Uartregs Uartregs;
Uartregs *uart3regs;

/*
 *  IRQ's defined by SA1100
 */
enum
{
	IRQgpio0=	0,
	IRQgpio1=	1,
	IRQgpio2=	2,
	IRQgpio3=	3,
	IRQgpio4=	4,
	IRQgpio5=	5,
	IRQgpio6=	6,
	IRQgpio7=	7,
	IRQgpio8=	8,
	IRQgpio9=	9,
	IRQgpio10=	10,
	IRQgpiohi=	11,
	IRQlcd=		12,
	IRQudc=		13,
	IRQuart1b=	15,
	IRQuart2=	16,
	IRQuart3=	17,
	IRQmcp=		18,
	IRQssp=		19,
	IRQdma0=	20,
	IRQdma1=	21,
	IRQdma2=	22,
	IRQdma3=	23,
	IRQdma4=	24,
	IRQdma5=	25,
	IRQtimer0=	26,
	IRQtimer1=	27,
	IRQtimer2=	28,
	IRQtimer3=	29,
	IRQsecond=	30,
	IRQrtc=		31,
};

/*
 *  GPIO lines (signal names from compaq document).  _i indicates input
 *  and _o output.
 */
enum
{
	PWR_ON_i=	1<<0,	/* power button */
	UP_IRQ_i=	1<<1,	/* microcontroller interrupts */
	LDD8_o=		1<<2,	/* LCD data 8-15 */
	LDD9_o=		1<<3,
	LDD10_o=	1<<4,
	LDD11_o=	1<<5,
	LDD12_o=	1<<6,
	LDD13_o=	1<<7,
	LDD14_o=	1<<8,
	LDD15_o=	1<<9,
	CARD_IND1_i=	1<<10,	/* card inserted in PCMCIA socket 1 */
	CARD_IRQ1_i=	1<<11,	/* PCMCIA socket 1 interrupt */
	CLK_SET0_o=	1<<12,	/* clock selects for audio codec */
	CLK_SET1_o=	1<<13,
	L3_SDA_io=	1<<14,	/* UDA1341 interface */
	L3_MODE_o=	1<<15,
	L3_SCLK_o=	1<<16,
	CARD_IND0_i=	1<<17,	/* card inserted in PCMCIA socket 0 */
	KEY_ACT_i=	1<<18,	/* hot key from cradle */
	SYS_CLK_i=	1<<19,	/* clock from codec */
	BAT_FAULT_i=	1<<20,	/* battery fault */
	CARD_IRQ0_i=	1<<21,	/* PCMCIA socket 0 interrupt */
	LOCK_i=		1<<22,	/* expansion pack lock/unlock */
	COM_DCD_i=	1<<23,	/* DCD from UART3 */
	OPT_IRQ_i=	1<<24,	/* expansion pack IRQ */
	COM_CTS_i=	1<<25,	/* CTS from UART3 */
	COM_RTS_oi=	1<<26,	/* RTS to UART3 */
	OPT_IND_i=	1<<27,	/* expansion pack inserted */
};
