#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 *  headland hip set for the safari.
 *  
 *  serious magic!!!
 */

enum
{
	Head=		0x92,		/* control port */
	 Reset=		(1<<0),		/* reset the 386 */
	 A20ena=	(1<<1),		/* enable address line 20 */
};

/*
 *  ports used by the DMA controllers
 */
typedef struct DMA	DMA;
struct DMA {
	uchar	addr[4];	/* current address (4 channels) */
	uchar	count[4];	/* current count (4 channels) */
	uchar	page[4];	/* page registers (4 channels) */
	uchar	cmd;		/* command status register */
	uchar	req;		/* request registers */
	uchar	sbm;		/* single bit mask register */
	uchar	mode;		/* mode register */
	uchar	cbp;		/* clear byte pointer */
	uchar	mc;		/* master clear */
	uchar	cmask;		/* clear mask register */
	uchar	wam;		/* write all mask register bit */
};

DMA dma[2] = {
	{ 0x00, 0x02, 0x04, 0x06,
	  0x01, 0x03, 0x05, 0x07,
	  0x87, 0x83, 0x81, 0x82,
	  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f },
	{ 0xc0, 0xc6, 0xca, 0xce,
	  0xc4, 0xc8, 0xcc, 0xcf,
	  0x80, 0x8b, 0x89, 0x8a,
	  0xd0, 0xd2, 0xd4, 0xd6, 0xd8, 0xda, 0xdc, 0xde },
};

/*
 *  enable address bit 20
 */
void
a20enable(void)
{
	outb(Head, A20ena);
}

/*
 *  reset the chip
 */
void
exit(void)
{
	int i;

	u = 0;
	print("exiting\n");
	outb(Head, Reset);
}

/*
 *  setup a dma transfer.  return count actually set up.  we DMA up
 *  to a page.
 */
long
dmasetup(int d, int chan, Page *pg, long len, int isread)
{
	DMA *dp;
	ulong addr;

	dp = &dma[d];
	addr = (ulong)a;
	addr &= ~KZERO;

	outb(dp->cbp, isread ? 0x46 : 0x4a);
	outb(dp->mode, isread ? 0x46 : 0x4a);
	outb(dp->addr, addr);
	outb(dp->addr, addr>>8);
	outb(dp->page, addr>>16);
	outb(dp->count, len-1);
	outb(dp->count, (len-1)>>8);
	outb(dp->sbm, 2);
}
