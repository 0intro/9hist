/*
 *  programmable interrupt vectors (for the 8259's)
 */
enum
{
	Faultvec=	14,		/* page fault */
	Int0vec=	16,		/* first 8259 */
	 Clockvec=	Int0vec+0,	/*  clock interrupts */
	 Kbdvec=	Int0vec+1,	/*  keyboard interrupts */
	 ComBvec=	Int0vec+3,	/*  inerrupt from uart b */
	 ComAvec=	Int0vec+4,	/*  inerrupt from uart a */
	 Floppyvec=	Int0vec+6,	/*  floppy interrupts */
	Int1vec=	Int0vec+8,	/* second 8259 */
	 Mousevec=	Int1vec+4,	/*  mouse interrupt */
	 Hardvec=	Int1vec+6,	/*  hard disk */
};
