/*
 *  8259 interrupt controllers
 */
enum
{
	Int0ctl=	0x20,		/* control port */
	Int0aux=	0x21,		/* everything else port */
	Int1ctl=	0xA0,		/* control port */
	Int1aux=	0xA1,		/* everything else port */

	Intena=		0x20,		/* written to Intctlport, enables next int */

	Int0vec=	16,		/* first interrupt vector used by the 8259 */
	Clockvec=	Int0vec+0,	/* clock interrupts */
	Kbdvec=		Int0vec+1,	/* keyboard interrupts */
};
#define	INT0ENABLE	outb(Int0ctl, Intena)
#define	INT1ENABLE	outb(Int1ctl, Intena)

/*
 *  8237 dma controllers
 */
enum
{
	/*
	 *  the byte registers for DMA0 are all one byte apart
	 */
	Dma0=		0x00,
	Dma0status=	Dma0+0x8,	/* status port */
	Dma0reset=	Dma0+0xD,	/* reset port */

	/*
	 *  the byte registers for DMA1 are all two bytes apart (why?)
	 */
	Dma1=		0xC0,
	Dma1status=	Dma1+2*0x8,	/* status port */
	Dma1reset=	Dma1+2*0xD,	/* reset port */
};
