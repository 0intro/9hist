#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"
#include	"sa1110dma.h"

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

typedef struct DMAchan {
	int		inuse;
	Rendez	r;
	void	(*intr)(void*, ulong);
	void	*param;
} DMAchan;

struct {
	Lock;
	DMAchan	chan[6];
} dma;

struct dmaregs {
	ulong	ddar;
	ulong	dcsr_set;
	ulong	dcsr_clr;
	ulong	dcsr_rd;
	void*	dstrtA;
	ulong	dxcntA;
	void*	dstrtB;
	ulong	dxcntB;
} *dmaregs;

static void	dmaintr(Ureg*, void *);

void
dmainit(void) {
	int i;

	/* map the lcd regs into the kernel's virtual space */
	dmaregs = (struct dmaregs*)mapspecial(DMAREGS, NDMA*sizeof(struct dmaregs));;
	for (i = 0; i < NDMA; i++) {
		intrenable(IRQdma0+i, dmaintr, &dmaregs[i], "DMA");
	}
}

int
dmaalloc(int rd, int bigendian, int burstsize, int datumsize, int device, ulong port, void (*intr)(void*, ulong), void *param) {
	int i;

	lock(&dma);
	for (i = 0; i < NDMA; i++) {
		if (dma.chan[i].inuse)
			continue;
		dma.chan[i].inuse++;
		unlock(&dma);
		dmaregs[i].ddar =
			(rd?1:0)<<RW |
			(bigendian?1:0)<<E |
			((burstsize==8)?1:0)<<BS |
			((datumsize==2)?1:0)<<DW |
			device<<DS |
			0x80000000 | ((ulong)port << 6);
		dma.chan[i].intr = intr;
		dma.chan[i].param = param;
		return i;
	}
	unlock(&dma);
	return -1;
}

void
dmafree(int i) {
	dma.chan[i].inuse = 0;
	dma.chan[i].intr = nil;
}

static int
dmaready(void *dcsr) {
	return *(int*)dcsr & ((1<<DONEA)|(1<<DONEB));
}

ulong
dmastart(int chan, void *addr, int count) {
	ulong status;

	if (((status = dmaregs[chan].dcsr_rd) & ((1<<DONEA)|(1<<DONEB))) == 0)
		return 0;

	cachewbregion((ulong)addr, count);
	if ((status & (1<<BIU | 1<<STRTB)) == (1<<BIU | 1<<STRTB) ||
				(status & (1<<BIU | 1<<STRTA)) == (1<<STRTA)) {
		dmaregs[chan].dcsr_clr |= 1<<DONEA | 1<<STRTA;
		dmaregs[chan].dstrtA = addr;
		dmaregs[chan].dxcntA = count-1;
		dmaregs[chan].dcsr_set |= 1<<RUN | 1<<IE | 1<<STRTA;
		return 1<<DONEA;
	} else {
		dmaregs[chan].dcsr_clr |= 1<<DONEB | 1<<STRTB;
		dmaregs[chan].dstrtB = addr;
		dmaregs[chan].dxcntB = count-1;
		dmaregs[chan].dcsr_set |= 1<<RUN | 1<<IE | 1<<STRTB;
		return 1<<DONEB;
	}
}

ulong
dmadone(int chan, ulong op) {
	ulong dcsr;

	dcsr = dmaregs[chan].dcsr_rd;
	if (dcsr & 1<<ERROR)
		pprint("DMA error, chan %d, status 0x%lux\n", chan, dcsr);
	return dcsr & (op | 1<<ERROR);
}

int
dmaidle(int chan) {
	ulong dcsr;

	dcsr = dmaregs[chan].dcsr_rd;
	if (dcsr & 1<<ERROR)
		pprint("DMA error, chan %d, status 0x%lux\n", chan, dcsr);
	return (dcsr & (1<<DONEA | 1<<DONEB)) == (1<<DONEA | 1<<DONEB);
}

void
dmawait(int chan, ulong op) {
	ulong dcsr;

	while (((dcsr = dmaregs[chan].dcsr_rd) & (op | 1<<ERROR)) == 0)
		sleep(&dma.chan[chan].r, dmaready, &dmaregs[chan].dcsr_rd);
	if (dcsr & 1<<ERROR)
		pprint("DMA error, chan %d, status 0x%lux\n", chan, dcsr);
}

/*
 *  interrupt routine
 */
static void
dmaintr(Ureg*, void *x)
{
	int i;
	struct dmaregs *regs = x;
	ulong dcsr;

	i = regs - dmaregs;
	if ((dcsr = regs->dcsr_rd) & (1<<DONEA | 1<<DONEB | 1<<ERROR)) {
		wakeup(&dma.chan[i].r);
		if (dma.chan[i].intr)
			(*dma.chan[i].intr)(dma.chan[i].param, dcsr);
	} else
		print("spurious DMA interrupt, channel %d, status 0x%lux\n", i, dcsr);
}
