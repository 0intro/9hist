/*
 *	EB164 and similar
 *	CPU:	21164
 *	Core Logic: 21172 CIA or 21174 PYXIS
  */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

static ulong *core;
static ulong *wind;

static ulong windsave[16];
static ulong coresave[1];

ulong	iobase0;
ulong	iobase1;
#define	iobase(p)	(iobase0+(p))

static int
ident(void)
{
	return 0;	/* bug! */
}

static uvlong* sgmap;

static void
sginit(void)
{
	ulong pa;
	uvlong *pte;

	sgmap = xspanalloc(BY2PG, BY2PG, 0);
	memset(sgmap, 0, BY2PG);

	pte = sgmap;
	for(pa = 0; pa < 8*1024*1024; pa += BY2PG)
		*pte++ = ((pa>>PGSHIFT)<<1)|1;

	wind[0x400/4] = ISAWINDOW|4|2|1;
	wind[0x440/4] = 0x00700000;
	wind[0x480/4] = PADDR(sgmap);
}

static void *
kmapio(ulong space, ulong offset, int size)
{
	return kmapv(((uvlong)space<<32LL)|offset, size);
}

static void
coreinit(void)
{
	int i;

	core = kmapio(0x87, 0x40000000, 0x10000);
	wind = kmapio(0x87, 0x60000000, 0x1000);

	iobase0 = (ulong)kmapio(0x89, 0, 0x20000);

	/* hae_io = core[0x440/4];
	iobase1 = (ulong)kmapio(0x89, hae_io, 0x10000); */

	/* save critical parts of hardware memory mapping */
	for (i = 4; i < 8; i++) {
		windsave[4*(i-4)+0] = wind[(i*0x100+0x00)/4];
		windsave[4*(i-4)+1] = wind[(i*0x100+0x40)/4];
		windsave[4*(i-4)+2] = wind[(i*0x100+0x80)/4];
	}
	coresave[0] = core[0x140/4];

	/* disable windows */
	wind[0x400/4] = 0;
	wind[0x500/4] = 0;
	wind[0x600/4] = 0;
	wind[0x700/4] = 0;

	/* direct map bottom 1G PCI target space to KZERO in window 1 */
	wind[0x500/4] = PCIWINDOW|1;
	wind[0x540/4] = 0x3ff00000;
	wind[0x580/4] = 0;

sginit();

	/* clear error state */
	core[0x8200/4] = 0x7ff;

	/* set config: byte/word enable, no monster window, etc. */
	core[0x140/4] = 0x21;

	/* turn off mcheck on master abort.  now we can probe PCI space. */
	core[0x8280/4] &= ~(1<<7);

	/* set up interrupts. */
	i8259init();
	cserve(52, 4);		/* enable SIO interrupt */
}

void
ciaerror(void)
{
	print("cia error 0x%luX\n", core[0x8200/4]);
}

static void
corehello(void)
{
	print("cpu%d: CIA revision %d; cnfg %lux cntrl %lux\n",
			0,	/* BUG */
			core[0x80/4] & 0x7f, core[0x140/4], core[0x100/4]);
	print("cpu%d: HAE_IO %lux\n", 0, core[0x440/4]);
	print("\n");
}

static void
coredetach(void)
{
	int i;

	for (i = 4; i < 8; i++) {
		wind[(i*0x100+0x00)/4] = windsave[4*(i-4)+0];
		wind[(i*0x100+0x40)/4] = windsave[4*(i-4)+1];
		wind[(i*0x100+0x80)/4] = windsave[4*(i-4)+2];
	}
	core[0x140/4] = coresave[0];
/*	for (i = 0; i < 4; i++)
		if (i != 4)
			cserve(53, i);		/* disable interrupts */
}

static Lock	pcicfgl;
static ulong	pcimap[256];

static void*
pcicfg2117x(int tbdf, int rno)
{
	int space, bus;
	ulong base;

	bus = BUSBNO(tbdf);
	lock(&pcicfgl);
	base = pcimap[bus];
	if (base == 0) {
		if(bus)
			space = 0x8B;
		else
			space = 0x8A;
		pcimap[bus] = base = (ulong)kmapio(space, MKBUS(0, bus, 0, 0), (1<<16));
	}
	unlock(&pcicfgl);
	return (void*)(base + BUSDF(tbdf) + rno);
}

static void*
pcimem2117x(int addr, int len)
{
	return kmapio(0x88, addr, len);
}

/*
 *	interrupts -- adapted from PC, needs work.
 */

static Lock irqctllock;
static Irqctl *irqctl[256];
static char irqmask[3];

static void
intr164(Ureg *ur)
{
	int i, v;
	Irqctl *ctl;
	Irq *irq;
	Mach *mach;

	v = (ulong)ur->a1>>4;
	if (v < 0x80) {
		iprint("unknown device intr v %d\n", v);
		return;
	}
	v -= 0x80;
	if(v < 256 && (ctl = irqctl[v])){
		if(ctl->isintr){
			m->intr++;
			if(ctl->isr)
				ctl->isr(v);
/*				if(v >= VectorPIC && v <= MaxVectorPIC)
				m->lastintr = v-VectorPIC; */
		}

		for(irq = ctl->irq; irq; irq = irq->next)
			irq->f(ur, irq->a);

		if(ctl->eoi)
			ctl->eoi(v);
	}
	else if(v >= VectorPIC && v <= MaxVectorPIC){
		/*
		 * An unknown interrupt.
		 * Check for a default IRQ7. This can happen when
		 * the IRQ input goes away before the acknowledge.
		 * In this case, a 'default IRQ7' is generated, but
		 * the corresponding bit in the ISR isn't set.
		 * In fact, just ignore all such interrupts.
		 */
		iprint("cpu%d: spurious interrupt %d, last %d",
			m->machno, v-VectorPIC, 0 /*m->lastintr*/);
		for(i = 0; i < 32; i++){
			if(!(active.machs & (1<<i)))
				continue;
			mach = MACHP(i);
			if(m->machno == mach->machno)
				continue;
			iprint(": cpu%d: last %d", mach->machno, 0 /*mach->lastintr*/);
		}
		iprint("\n");
/*			m->spuriousintr++; */
		return;
	}
	else{
		dumpregs(ur);
		panic("unknown intr: %d\n", v); /* */
	}
}

static int
intrenable164(int v, void (*f)(Ureg*, void*), void*a, int tbdf)
{
	Irq * irq;
	Irqctl *ctl;

	lock(&irqctllock);
	if(irqctl[v] == 0){
		ctl = xalloc(sizeof(Irqctl));
/* this is all wrong; FIXME! */
		if(BUSTYPE(tbdf) == BusPCI)
			cserve(52, v-VectorPCI);
		else if(v >= VectorPIC && i8259enable(v, tbdf, ctl) == -1){
			unlock(&irqctllock);
			iprint("intrenable: didn't find v %d, tbdf 0x%uX\n", v, tbdf);
			xfree(ctl);
			return -1;
		}
		irqctl[v] = ctl;
	}
	ctl = irqctl[v];
	irq = xalloc(sizeof(Irq));
	irq->f = f;
	irq->a = a;
	irq->next = ctl->irq;
	ctl->irq = irq;
	unlock(&irqctllock);
	return 0;
}

/*
 *	I have a function pointer in PCArch for every one of these, because on
 *	some Alphas we have to use sparse mode, but on others we can use
 *	MOVB et al.  Additionally, the PC164 documentation threatened us
 *	with the lie that the SIO is in region B, but everything else in region A.
 *	This turned out not to be the case.  Given the cost of this solution, it
 *	may be better just to use sparse mode for I/O space on all platforms.
 */
int
inb2117x(int port)
{
	mb();
	return *(uchar*)(iobase(port));
}

ushort
ins2117x(int port)
{
	mb();
	return *(ushort*)(iobase(port));
}

ulong
inl2117x(int port)
{
	mb();
	return *(ulong*)(iobase(port));
}

void
outb2117x(int port, int val)
{
	mb();
	*(uchar*)(iobase(port)) = val;
	mb();
}

void
outs2117x(int port, ushort val)
{
	mb();
	*(ushort*)(iobase(port)) = val;
	mb();
}

void
outl2117x(int port, ulong val)
{
	mb();
	*(ulong*)(iobase(port)) = val;
	mb();
}

void
insb2117x(int port, void *buf, int len)
{
	int i;
	uchar *p, *q;

	p = (uchar*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*q++ = *p;
	}
}

void
inss2117x(int port, void *buf, int len)
{
	int i;
	ushort *p, *q;

	p = (ushort*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*q++ = *p;
	}
}

void
insl2117x(int port, void *buf, int len)
{
	int i;
	ulong *p, *q;

	p = (ulong*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*q++ = *p;
	}
}

void
outsb2117x(int port, void *buf, int len)
{
	int i;
	uchar *p, *q;

	p = (uchar*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*p = *q++;
	}
}

void
outss2117x(int port, void *buf, int len)
{
	int i;
	ushort *p, *q;

	p = (ushort*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*p = *q++;
	}
}

void
outsl2117x(int port, void *buf, int len)
{
	int i;
	ulong *p, *q;

	p = (ulong*)iobase(port);
	q = buf;
	for(i = 0; i < len; i++){
		mb();
		*p = *q++;
	}
}

PCArch arch164 = {
	"EB164",
	ident,
	coreinit,
	corehello,
	coredetach,
	pcicfg2117x,
	pcimem2117x,
	intr164,
	intrenable164,

	inb2117x,
	ins2117x,
	inl2117x,
	outb2117x,
	outs2117x,
	outl2117x,
	insb2117x,
	inss2117x,
	insl2117x,
	outsb2117x,
	outss2117x,
	outsl2117x,
};
