/*
 *  interrupt levels
 */
enum {
	Clockvec=	8,
	Kbdvec=		9,
	Floppyvec=	14,
};

/*
 *  damned 8259, assume DOS sets it up for us
 */
enum {
	Intctlport=	0x20,	/* 8259 control port */
	Intctlmask=	0x21,	/* interrupt mask */

	Intenable=	0x20,	/* written to Intctlport, enables next int */
};

#define	INTENABLE	outb(Intctlport, Intenable)
