#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"
#include	"sa1110dma.h"

static int debug = 1;

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
	int		allocated;
	Ref		active;
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
	if (debug) print("dma: dmaalloc registers 0x%ux mapped at 0x%p\n",
		DMAREGS, dmaregs);
	for (i = 0; i < NDMA; i++) {
		intrenable(IRQdma0+i, dmaintr, &dmaregs[i], "DMA");
	}
}

int
dmaalloc(int rd, int bigendian, int burstsize, int datumsize, int device, ulong port, void (*intr)(void*, ulong), void *param) {
	int i;
	ulong ddar;

	lock(&dma);
	for (i = 0; i < NDMA; i++) {
		if (dma.chan[i].allocated)
			continue;
		dma.chan[i].allocated++;
		unlock(&dma);
		ddar =
			(rd?1:0)<<RW |
			(bigendian?1:0)<<E |
			((burstsize==8)?1:0)<<BS |
			((datumsize==2)?1:0)<<DW |
			device<<DS |
			0x80000000 | ((ulong)port << 6);
		dmaregs[i].ddar = ddar;
		if (debug) print("dma: dmaalloc: 0x%lux\n", ddar);
		dma.chan[i].intr = intr;
		dma.chan[i].param = param;
		dmaregs[i].dcsr_clr = 0xff;
		return i;
	}
	unlock(&dma);
	return -1;
}

void
dmafree(int i) {
	dma.chan[i].allocated = 0;
	dma.chan[i].intr = nil;
}

ulong
dmastart(int chan, void *addr, int count) {
	ulong status;

	status = dmaregs[chan].dcsr_rd;
	if (debug > 1) print("dma: dmastart 0x%lux\n", status);

	if (dma.chan[chan].active.ref >= 2) {
		if (debug > 1) print("\n");
		return 0;
	}

	if ((status & (1<<DONEA|1<<DONEB|1<<RUN)) == 1<<RUN)
		panic("dmastart called while busy");

	cachewbregion((ulong)addr, count);
	if ((status & (1<<BIU | 1<<STRTB)) == (1<<BIU | 1<<STRTB) ||
		(status & (1<<BIU | 1<<STRTA)) == 0) {
		dmaregs[chan].dcsr_clr = 1<<DONEA | 1<<STRTA;
		dmaregs[chan].dstrtA = addr;
		dmaregs[chan].dxcntA = count-1;
		incref(&dma.chan[chan].active);
		dmaregs[chan].dcsr_set = 1<<RUN | 1<<IE | 1<<STRTA;
		return 1<<DONEA;
	} else {
		dmaregs[chan].dcsr_clr = 1<<DONEB | 1<<STRTB;
		dmaregs[chan].dstrtB = addr;
		dmaregs[chan].dxcntB = count-1;
		incref(&dma.chan[chan].active);
		dmaregs[chan].dcsr_set = 1<<RUN | 1<<IE | 1<<STRTB;
		return 1<<DONEB;
	}
}

int
dmaidle(int chan) {
	return dma.chan[chan].active.ref == 0;
}

static int
_dmaidle(void* chan) {
	return dma.chan[(int)chan].active.ref == 0;
}

void
dmawait(int chan) {
	while (dma.chan[chan].active.ref)
		sleep(&dma.chan[chan].r, _dmaidle, (void*)chan);
}

/*
 *  interrupt routine
 */
static void
dmaintr(Ureg*, void *x)
{
	int i;
	struct dmaregs *regs = x;
	ulong dcsr, donebit;

	i = regs - dmaregs;
	dcsr = regs->dcsr_rd;
	if (debug > 1)
		iprint("dma: interrupt channel %d, status 0x%lux\n", i, dcsr);
	if (dcsr & 1<<ERROR)
		iprint("error, channel %d, status 0x%lux\n", i, dcsr);
	donebit = 1<<((dcsr&1<<BIU)?DONEA:DONEB);
	if (dcsr & donebit) {
		regs->dcsr_clr |= donebit;
		if (dma.chan[i].intr)
			(*dma.chan[i].intr)(dma.chan[i].param, 3-dma.chan[i].active.ref);
		/* must call interrupt routine before calling decref */
		decref(&dma.chan[i].active);
		wakeup(&dma.chan[i].r);
	} else
		iprint("spurious DMA interrupt, channel %d, status 0x%lux\n", i, dcsr);
}
