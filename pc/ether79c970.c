/*
 * AM79C970
 * PCnet-PCI Single-Chip Ethernet Controller for PCI Local Bus
 * To do:
 *	only issue transmit interrupt if necessary?
 *	dynamically increase rings as necessary?
 *	use Block's as receive buffers?
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"

#include "etherif.h"

enum {
	Lognrdre	= 7,
	Nrdre		= (1<<Lognrdre),	/* receive descriptor ring entries */
	Logntdre	= 5,
	Ntdre		= (1<<Logntdre),	/* transmit descriptor ring entries */

	Rbsize		= ETHERMAXTU+4,		/* ring buffer size (+4 for CRC) */
};

enum {						/* DWIO I/O resource map */
	Aprom		= 0x0000,		/* physical address */
	Rdp		= 0x0010,		/* register data port */
	Rap		= 0x0014,		/* register address port */
	Sreset		= 0x0018,		/* software reset */
	Bdp		= 0x001C,		/* bus configuration register data port */
};

enum {						/* CSR0 */
	Init		= 0x0001,		/* begin initialisation */
	Strt		= 0x0002,		/* enable chip */
	Stop		= 0x0004,		/* disable chip */
	Tdmd		= 0x0008,		/* transmit demand */
	Txon		= 0x0010,		/* transmitter on */
	Rxon		= 0x0020,		/* receiver on */
	Iena		= 0x0040,		/* interrupt enable */
	Intr		= 0x0080,		/* interrupt flag */
	Idon		= 0x0100,		/* initialisation done */
	Tint		= 0x0200,		/* transmit interrupt */
	Rint		= 0x0400,		/* receive interrupt */
	Merr		= 0x0800,		/* memory error */
	Miss		= 0x1000,		/* missed frame */
	Cerr		= 0x2000,		/* collision */
	Babl		= 0x4000,		/* transmitter timeout */
	Err		= 0x8000,		/* Babl|Cerr|Miss|Merr */
};
	
enum {						/* CSR3 */
	Bswp		= 0x0004,		/* byte swap */
	Emba		= 0x0008,		/* enable modified back-off algorithm */
	Dxmt2pd		= 0x0010,		/* disable transmit two part deferral */
	Lappen		= 0x0020,		/* look-ahead packet processing enable */
	Idonm		= 0x0100,		/* initialisation done mask */
	Tintm		= 0x0200,		/* transmit interrupt mask */
	Rintm		= 0x0400,		/* receive interrupt mask */
	Merrm		= 0x0800,		/* memory error mask */
	Missm		= 0x1000,		/* missed frame mask */
	Bablm		= 0x4000,		/* babl mask */
};

enum {						/* CSR4 */
	ApadXmt		= 0x0800,		/* auto pad transmit */
};

enum {						/* CSR15 */
	Prom		= 0x8000,		/* promiscuous mode */
};

typedef struct {				/* Initialisation Block */
	ushort	mode;
	uchar	rlen;				/* upper 4 bits */
	uchar	tlen;				/* upper 4 bits */
	uchar	padr[6];
	uchar	res[2];
	uchar	ladr[8];
	ulong	rdra;
	ulong	tdra;
} Iblock;

typedef struct {				/* receive descriptor ring entry */
	ulong	rbadr;
	ulong	rmd1;				/* status|bcnt */
	ulong	rmd2;				/* rcc|rpc|mcnt */
	ulong	rmd3;				/* reserved */
} Rdre;

typedef struct {				/* transmit descriptor ring entry */
	ulong	tbadr;
	ulong	tmd1;				/* status|bcnt */
	ulong	tmd2;				/* errors */
	ulong	tmd3;				/* reserved */
} Tdre;

enum {						/* [RT]dre status bits */
	Enp		= 0x01000000,		/* end of packet */
	Stp		= 0x02000000,		/* start of packet */
	RxBuff		= 0x04000000,		/* buffer error */
	TxDef		= 0x04000000,		/* deferred */
	RxCrc		= 0x08000000,		/* CRC error */
	TxOne		= 0x08000000,		/* one retry needed */
	RxOflo		= 0x10000000,		/* overflow error */
	TxMore		= 0x10000000,		/* more than one retry needed */
	Fram		= 0x20000000,		/* framing error */
	RxErr		= 0x40000000,		/* Fram|Oflo|Crc|RxBuff */
	TxErr		= 0x40000000,		/* Uflo|Lcol|Lcar|Rtry */
	Own		= 0x80000000,
};

typedef struct {
	Lock	raplock;			/* registers other than CSR0 */

	Iblock	iblock;

	Rdre*	rdr;				/* receive descriptor ring */
	void*	rrb;				/* receive ring buffers */
	int	rdrx;				/* index into rdr */

	Rendez	trendez;			/* wait here for free tdre */
	Tdre*	tdr;				/* transmit descriptor ring */
	void*	trb;				/* transmit ring buffers */
	int	tdrx;				/* index into tdr */
} Ctlr;

static void
attach(Ether* ether)
{
	int port;

	port = ether->port;
	outl(port+Rdp, Iena|Strt);
}

static void
ringinit(Ctlr* ctlr)
{
	int i, x;

	/*
	 * Initialise the receive and transmit buffer rings. The ring
	 * entries must be aligned on 16-byte boundaries.
	 */
	if(ctlr->rdr == 0)
		ctlr->rdr = xspanalloc(Nrdre*sizeof(Rdre), 0x10, 0);
	if(ctlr->rrb == 0)
		ctlr->rrb = xalloc(Nrdre*Rbsize);
	x = PADDR(ctlr->rrb);
	for(i = 0; i < Nrdre; i++){
		ctlr->rdr[i].rbadr = x;
		x += Rbsize;
		ctlr->rdr[i].rmd1 = Own|(-Rbsize & 0xFFFF);
	}
	ctlr->rdrx = 0;

	if(ctlr->tdr == 0)
		ctlr->tdr = xspanalloc(Ntdre*sizeof(Tdre), 0x10, 0);
	if(ctlr->trb == 0)
		ctlr->trb = xalloc(Ntdre*Rbsize);
	x = PADDR(ctlr->trb);
	for(i = 0; i < Ntdre; i++){
		ctlr->tdr[i].tbadr = x;
		x += Rbsize;
	}
	ctlr->tdrx = 0;
}

static void
promiscuous(void* arg, int on)
{
	Ether *ether;
	int port, x;
	Ctlr *ctlr;

	ether = arg;
	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * Put the chip into promiscuous mode. First we must wait until
	 * anyone transmitting is done, then we can stop the chip and put
	 * it in promiscuous mode. Restarting is made harder by the chip
	 * reloading the transmit and receive descriptor pointers with their
	 * base addresses when Strt is set (unlike the older Lance chip),
	 * so the rings must be re-initialised.
	 */
	qlock(&ether->tlock);

	ilock(&ctlr->raplock);
	outl(port+Rdp, Stop);

	outl(port+Rap, 15);
	x = inl(port+Rdp) & ~Prom;
	if(on)
		x |= Prom;
	outl(port+Rdp, x);
	outl(port+Rap, 0);

	ringinit(ctlr);
	outl(port+Rdp, Iena|Strt);
	iunlock(&ctlr->raplock);

	qunlock(&ether->tlock);
}

static int
owntdre(void* arg)
{
	return (((Tdre*)arg)->tmd1 & Own) == 0;
}

static long
write(Ether* ether, void* buf, long n)
{
	int port;
	Ctlr *ctlr;
	Tdre *tdre;
	Etherpkt *pkt;

	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * Wait for a transmit ring descriptor (and hence a buffer) to become
	 * free. If none become free after a reasonable period, give up.
	 */
	tdre = &ctlr->tdr[ctlr->tdrx];
	tsleep(&ctlr->trendez, owntdre, tdre, 100);
	if(owntdre(tdre) == 0)
		return 0;

	/*
	 * Copy the packet to the transmit buffer and fill in our
	 * source ethernet address. There's no need to pad to ETHERMINTU
	 * here as we set ApadXmit in CSR4.
	 */
	pkt = KADDR(tdre->tbadr);
	memmove(pkt->d, buf, n);
	memmove(pkt->s, ether->ea, sizeof(pkt->s));

	/*
	 * Give ownership of the descriptor to the chip, increment the
	 * software ring descriptor pointer and tell the chip to poll.
	 */
	tdre->tmd2 = 0;
	tdre->tmd1 = Own|Stp|Enp|(-n & 0xFFFF);
	ctlr->tdrx = NEXT(ctlr->tdrx, Ntdre);
	outl(port+Rdp, Iena|Tdmd);

	ether->outpackets++;
	return n;
}

static void
interrupt(Ureg*, void* arg)
{
	Ether *ether;
	int port, csr0, status;
	Ctlr *ctlr;
	Rdre *rdre;
	Etherpkt *pkt;

	ether = arg;
	port = ether->port;
	ctlr = ether->ctlr;

	/*
	 * Acknowledge all interrupts and whine about those that shouldn't
	 * happen.
	 */
	csr0 = inl(port+Rdp);
	outl(port+Rdp, Babl|Cerr|Miss|Merr|Rint|Tint|Iena);
	if(csr0 & (Babl|Miss|Merr))
		print("AMD70C970#%d: csr0 = 0x%uX\n", ether->ctlrno, csr0);

	/*
	 * Receiver interrupt: run round the descriptor ring logging
	 * errors and passing valid receive data up to the higher levels
	 * until we encounter a descriptor still owned by the chip.
	 */
	if(csr0 & Rint){
		rdre = &ctlr->rdr[ctlr->rdrx];
		while(((status = rdre->rmd1) & Own) == 0){
			if(status & RxErr){
				if(status & RxBuff)
					ether->buffs++;
				if(status & RxCrc)
					ether->crcs++;
				if(status & RxOflo)
					ether->overflows++;
			}
			else{
				ether->inpackets++;
				pkt = KADDR(rdre->rbadr);
				etherrloop(ether, pkt, (rdre->rmd2 & 0x0FFF)-4);
			}

			/*
			 * Finished with this descriptor, reinitialise it,
			 * give it back to the chip, then on to the next...
			 */
			rdre->rmd2 = 0;
			rdre->rmd1 = Own|(-Rbsize & 0xFFFF);

			ctlr->rdrx = NEXT(ctlr->rdrx, Nrdre);
			rdre = &ctlr->rdr[ctlr->rdrx];
		}
	}

	/*
	 * Transmitter interrupt: wakeup anyone waiting for a free descriptor.
	 */
	if(csr0 & Tint)
		wakeup(&ctlr->trendez);
}

typedef struct Adapter Adapter;
struct Adapter {
	Adapter*	next;
	int		port;
	PCIcfg*		pcicfg;
};
static Adapter *adapter;

static PCIcfg*
amd79c90(Ether* ether)
{
	PCIcfg *pcicfg;
	static int devno = 0;
	int port;
	Adapter *ap;

	pcicfg = malloc(sizeof(PCIcfg));
	for(;;){
		pcicfg->vid = 0x1022;
		pcicfg->did = 0x2000;
		if((devno = pcimatch(0, devno, pcicfg)) == -1)
			break;

		port = pcicfg->baseaddr[0] & ~0x01;
		if(ether->port == 0 || ether->port == port)
			return pcicfg;

		ap = malloc(sizeof(Adapter));
		ap->pcicfg = pcicfg;
		ap->port = port;
		ap->next = adapter;
		adapter = ap;

		pcicfg = malloc(sizeof(PCIcfg));
	}
	free(pcicfg);

	return 0;
}

static int
reset(Ether* ether)
{
	int port, x;
	PCIcfg *pcicfg;
	Adapter *ap, **app;
	uchar ea[Eaddrlen];
	Ctlr *ctlr;

	/*
	 * Any adapter matches if no ether->port is supplied, otherwise the
	 * ports must match. First see if we've already found an adapter that fits
	 * the bill. If not then scan for another.
	 */
	port = 0;
	pcicfg = 0;
	for(app = &adapter, ap = *app; ap; app = &ap->next, ap = ap->next){
		if(ether->port == 0 || ether->port == ap->port){
			port = ap->port;
			pcicfg = ap->pcicfg;
			*app = ap->next;
			free(ap);
			break;
		}
	}
	if(port == 0 && (pcicfg = amd79c90(ether))){
		port = pcicfg->baseaddr[0] & ~0x01;
		ether->irq = pcicfg->irq;
	}
	if(port == 0)
		return -1;

	if(pcicfg)
		free(pcicfg);

	/*
	 * How can we tell what mode we're in at this point - if we're in WORD
	 * mode then the only 32-bit access we are allowed to make is a write to
	 * the RDP, which forces the chip to DWORD mode; if we're in DWORD mode
	 * then we're not allowed to make any non-DWORD accesses?
	 * Assuming we do a DWORD write to the RDP, how can we tell what we're
	 * about to overwrite as we can't reliably access the RAP?
	 *
	 * Force DWORD mode by writing to RDP, doing a reset then writing to RDP
	 * again; at least we know what state we're in now. The value of RAP after
	 * a reset is 0, so the second DWORD write will be to CSR0.
	 * Set the software style in BCR20 to be PCnet-PCI to ensure 32-bit access.
	 * Set the auto pad transmit in CSR4.
	 */
	outl(port+Rdp, 0x00);
	inl(port+Sreset);
	outl(port+Rdp, Stop);

	outl(port+Rap, 20);
	outl(port+Bdp, 0x0002);

	outl(port+Rap, 4);
	x = inl(port+Rdp) & 0xFFFF;
	outl(port+Rdp, ApadXmt|x);

	outl(port+Rap, 0);

	/*
	 * Check if we are going to override the adapter's station address.
	 * If not, read it from the I/O-space and set in ether->ea prior to loading the
	 * station address in the initialisation block.
	 */
	memset(ea, 0, Eaddrlen);
	if(memcmp(ea, ether->ea, Eaddrlen) == 0){
		x = inl(port+Aprom);
		ether->ea[0] = x & 0xFF;
		ether->ea[1] = (x>>8) & 0xFF;
		ether->ea[2] = (x>>16) & 0xFF;
		ether->ea[3] = (x>>24) & 0xFF;
		x = inl(port+Aprom+4);
		ether->ea[4] = x & 0xFF;
		ether->ea[5] = (x>>8) & 0xFF;
	}

	/*
	 * Allocate a controller structure and start to fill in the
	 * initialisation block (must be DWORD aligned).
	 */
	ether->ctlr = malloc(sizeof(Ctlr));
	ctlr = ether->ctlr;

	ctlr->iblock.rlen = Lognrdre<<4;
	ctlr->iblock.tlen = Logntdre<<4;
	memmove(ctlr->iblock.padr, ether->ea, sizeof(ctlr->iblock.padr));

	ringinit(ctlr);
	ctlr->iblock.rdra = PADDR(ctlr->rdr);
	ctlr->iblock.tdra = PADDR(ctlr->tdr);

	/*
	 * Point the chip at the initialisation block and tell it to go.
	 * Mask the Idon interrupt and poll for completion. Strt and interrupt
	 * enables will be set later when we're ready to attach to the network.
	 */
	x = PADDR(&ctlr->iblock);
	outl(port+Rap, 1);
	outl(port+Rdp, x & 0xFFFF);
	outl(port+Rap, 2);
	outl(port+Rdp, (x>>16) & 0xFFFF);
	outl(port+Rap, 3);
	outl(port+Rdp, Idonm);
	outl(port+Rap, 0);
	outl(port+Rdp, Init);

	while((inl(port+Rdp) & Idon) == 0)
		;
	outl(port+Rdp, Idon|Stop);
	
	ether->port = port;
	ether->attach = attach;
	ether->write = write;
	ether->interrupt = interrupt;

	ether->promiscuous = promiscuous;
	ether->arg = ether;

	return 0;
}

void
ether79c970link(void)
{
	addethercard("AMD79C970",  reset);
}
