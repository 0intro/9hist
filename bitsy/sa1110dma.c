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
	Lock;
	int		allocated;
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
	ulong status, n;

	ilock(&dma.chan[chan]);
	status = dmaregs[chan].dcsr_rd;
	if (debug > 1)
		iprint("dma: dmastart 0x%lux\n", status);

	if ((status & (1<<STRTA|1<<STRTB|1<<RUN)) == (1<<STRTA|1<<STRTB|1<<RUN)) {
		iunlock(&dma.chan[chan]);
		return 0;
	}
	cachewbregion((ulong)addr, count);
	n = 1;
	if ((status & (1<<BIU | 1<<STRTB)) == (1<<BIU | 1<<STRTB) ||
		(status & (1<<BIU | 1<<STRTA)) == 0) {
		assert((status & 1<<STRTA) == 0);
		if (status & 1<<RUN)
			n = 2;
		dmaregs[chan].dstrtA = addr;
		dmaregs[chan].dxcntA = count-1;
		dmaregs[chan].dcsr_set = 1<<RUN | 1<<IE | 1<<STRTA;
	} else {
		assert((status & 1<<STRTB) == 0);
		if (status & 1<<RUN)
			n = 2;
		dmaregs[chan].dstrtB = addr;
		dmaregs[chan].dxcntB = count-1;
		dmaregs[chan].dcsr_set = 1<<RUN | 1<<IE | 1<<STRTB;
	}
	iunlock(&dma.chan[chan]);
	return n;
}

int
dmaidle(int chan) {
	ulong status;

	status = dmaregs[chan].dcsr_rd;
	return (status & (1<<STRTA|1<<STRTB|1<<RUN)) == 0;
}

static int
_dmaidle(void* chan) {
	ulong status;

	status = dmaregs[(int)chan].dcsr_rd;
	return (status & (1<<STRTA|1<<STRTB|1<<RUN)) == 0;
}

void
dmawait(int chan) {
	while (!dmaidle(chan))
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
	ilock(&dma.chan[i]);
	dcsr = regs->dcsr_rd;
	if (debug > 1)
		iprint("dma: interrupt channel %d, status 0x%lux\n", i, dcsr);
	if (dcsr & 1<<ERROR)
		iprint("error, channel %d, status 0x%lux\n", i, dcsr);
	donebit = 1<<((dcsr&1<<BIU)?DONEA:DONEB);
	if (dcsr & donebit) {
		regs->dcsr_clr = donebit;
		/* try the other bit as well */
		donebit ^= 1<<DONEA|1<<DONEB;
		if (dcsr & donebit) {
			regs->dcsr_clr = donebit;
		}
		iunlock(&dma.chan[i]);
		if (dma.chan[i].intr) {
			dcsr &= 1<<STRTA | 1<<STRTB | 1<<RUN;
			(*dma.chan[i].intr)(dma.chan[i].param, dcsr == 0);
		}
		wakeup(&dma.chan[i].r);
		return;
	}
	iprint("spurious DMA interrupt, channel %d, status 0x%lux\n", i, dcsr);
	iunlock(&dma.chan[i]);
}
