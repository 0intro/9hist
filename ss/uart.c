#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

enum
{
	/* wr 0 */
	ResExtPend=	2<<3,
	ResTxPend=	5<<3,
	ResErr=		6<<3,

	/* wr 1 */
	TxIntEna=	1<<1,
	RxIntDis=	0<<3,
	RxIntFirstEna=	1<<3,
	RxIntAllEna=	2<<3,

	/* wr 3 */
	RxEna=		1,
	Rx5bits=	0<<6,
	Rx7bits=	1<<6,
	Rx6bits=	2<<6,
	Rx8bits=	3<<6,

	/* wr 4 */
	SyncMode=	0<<2,
	Rx1stop=	1<<2,
	Rx1hstop=	2<<2,
	Rx2stop=	3<<2,
	X16=		1<<6,

	/* wr 5 */
	TxRTS=		1<<1,
	TxEna=		1<<3,
	TxBreak=	1<<4,
	TxDTR=		1<<7,
	Tx5bits=	0<<5,
	Tx7bits=	1<<5,
	Tx6bits=	2<<5,
	Tx8bits=	3<<5,

	/* wr 9 */
	IntEna=		1<<3,
	ResetB=		1<<6,
	ResetA=		2<<6,
	HardReset=	3<<6,

	/* wr 11 */
	TRxCOutBR=	2,
	TxClockBR=	2<<3,
	RxClockBR=	2<<5,
	TRxCOI=		1<<2,

	/* wr 14 */
	BREna=		1,
	BRSource=	2,

	/* rr 0 */
	RxReady=	1,
	TxReady=	1<<2,
	RxDCD=		1<<3,
	RxCTS=		1<<5,
	RxBreak=	1<<7,

	/* rr 3 */
	ExtPendB=	1,	
	TxPendB=	1<<1,
	RxPendB=	1<<2,
	ExtPendA=	1<<3,	
	TxPendA=	1<<4,
	RxPendA=	1<<5,
};

typedef struct SCC	SCC;
struct SCC
{
	uchar	*ptr;		/* command/pointer register in Z8530 */
	uchar	*data;		/* data register in Z8530 */
};
SCC	scc[2];
#define	SCCV	(0x0000)	/* was 0x0F000000-0x1000 */
#define	SCCP	0xF1000000

#define PRINTING	0x4
#define MASK		0x1

void
sccsetup(void)
{
	ulong pte;

	pte = 	PTEVALID |
		PTEWRITE |
		PTEKERNEL |
		PTENOCACHE |
		PTEIO |
		((SCCP>>PGSHIFT)&0xFFFF)
		;
	putpmeg(SCCV, pte);
	scc[0].ptr = (uchar*)(SCCV+0);
	scc[0].data = (uchar*)(SCCV+2);
	scc[1].ptr = (uchar*)(SCCV+4);
	scc[1].data = (uchar*)(SCCV+6);
}

/*
 *  Access registers using the pointer in register 0.
 *  This is a bit stupid when accessing register 0.
 */
void
sccwrreg(SCC *sp, int addr, int value)
{
	*sp->ptr = addr;
}

ushort
sccrdreg(SCC *sp, int addr)
{
	*sp->ptr = addr;
	return *sp->ptr;
}


/*
 *  non-spooling non-interrupting get and put
 */
int
sccgetc(void)
{
	uchar ch;
	SCC *sp;

	sp = &scc[1];
	while((*sp->ptr&RxReady) == 0)
		;
	return *sp->data;
}
void
sccputc(int ch)
{
	SCC *sp;

	sp = &scc[1];
	while((*sp->ptr&TxReady)==0)
		;
	*sp->data = ch;
}
void
sccputs(char *s)
{
	while(*s)
		sccputc(*s++);
}
