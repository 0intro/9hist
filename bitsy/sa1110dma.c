#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

/*
 *	DMA helper routines
 */

enum {
	NDMA	=	6,			/* Number of DMA channels */
	DMAREGS	=	0xb0000000,	/* DMA registers, physical */
};

enum {
	/* Device Address Register, DDAR */
	RW		=	0,
	E		=	1,
	BS		=	2,
	DW		=	3,
	DS		=	4,	/* bits 4 - 7 */
	DA		=	8	/* bits 8 - 31 */
};

enum {
	/* Device Control & Status Register, DCSR */
	RUN		=	0,
	IE		=	1,
	ERROR	=	2,
	DONEA	=	3,
	STRTA	=	4,
	DONEB	=	5,
	STRTB	=	6,
	BIU		=	7
};

struct {
	Lock;
	Rendez	r;
	int		channels;
} dma;

struct dmaregs {
	ulong	ddar;
	ulong	dcsr_set;
	ulong	dcsr_clr;
	ulong	dcsr_rd;
	ulong	dstrtA;
	ulong	dxcntA;
	ulong	dstrtB;
	ulong	dxcntB;
} *dmaregs;

void
dmainit(void) {
	/* map the lcd regs into the kernel's virtual space */
	dmaregs = (struct dmaregs*)mapspecial(DMAREGS, NDMA*sizeof(struct dmaregs));;
}

int
dmaalloc(int rd, int bigendian, int burstsize, int datumsize, int device, void *port) {
	int i;

	lock(&dma);
	for (i = 0; i < NDMA; i++) {
		if (dma.channels & (1 << i))
			continue;
		dma.channels |= 1 << i;
		unlock(&dma);
		dmaregs[i].ddar =
			(rd?1:0)<<RW |
			(bigendian?1:0)<<E |
			((burstsize==8)?1:0)<<BS |
			((datumsize==2)?1:0)<<DW |
			device<<DS |
			0x80000000 | (port << 6);
		return i;
	}
	unlock(&dma);
	return -1;
}

void
dmafree(i) {
	int i;

	lock(&dma);
	dma.channels &= ~(1<<i);
	unlock(&dma);
}

static int
dmaready(void *dcsr) {

	return = *(ulong*)dcsr & ((1<<DONEA)|(1<<DONEB));
}

int
dmastart(int chan, void *addr, int count) {
	ulong ab;

	while ((ab = dmaready(&dma[chan].dcsr_rd)) == 0) {
		sleep(&dma.r, dmaready, &dma[chan].dcsr_rd);
	}
	cachewb();
	if (ab & (1<<DONEA)) {
		dma[chan].dcsr_clr |= 1<<DONEA | 1<<STARTA;
		dma[chan].dstrtA = addr;
		dma[chan].dxcntA = count-1;
		dma[chan].dcsr_set |= 1<<RUN | 1<<IE | 1<<STARTA;
	} else {
		dma[chan].dcsr_clr |= 1<<DONEB | 1<<STARTB;
		dma[chan].dstrtB = addr;
		dma[chan].dxcntB = count-1;
		dma[chan].dcsr_set |= 1<<RUN | 1<<IE | 1<<STARTB;
	}
}

/*
 *  interrupt routine
 */
static void
dmaintr(Ureg*, void *x)
{
	for (i = 0; i < NDMA; i++) {

	}
}
