/*
 *  programmable interrupt vectors (for the 8259)
 */
enum
{
	Faultvec=	14,		/* page fault */
	Int0vec=	16,		/* first interrupt vector used by the 8259 */
	Clockvec=	Int0vec+0,	/* clock interrupts */
	Kbdvec=		Int0vec+1,	/* keyboard interrupts */
	Mousevec=	Int0vec+12,	/* mouse interrupt */
};

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
