#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

#include "mp.h"
#include "apbootstrap.h"

extern int elcr;

static Bus* mpbus;
static Bus* mpbuslast;
static Apic mpapic[MaxAPICNO+1];
static int machno2apicno[MaxAPICNO+1];	/* inverse map: machno -> APIC ID */
static Lock mprdthilock;
static int mprdthi;

static char* buses[] = {
	"CBUSI ",
	"CBUSII",
	"EISA  ",
	"FUTURE",
	"INTERN",
	"ISA   ",
	"MBI   ",
	"MBII  ",
	"MCA   ",
	"MPI   ",
	"MPSA  ",
	"NUBUS ",
	"PCI   ",
	"PCMCIA",
	"TC    ",
	"VL    ",
	"VME   ",
	"XPRESS",
	0,
};

static Apic*
mkprocessor(PCMPprocessor* p)
{
	Apic *apic;

	if(!(p->flags & PcmpEN) || p->apicno > MaxAPICNO)
		return 0;

	apic = &mpapic[p->apicno];
	apic->type = PcmpPROCESSOR;
	apic->apicno = p->apicno;
	apic->flags = p->flags;
	apic->vecbase = VectorLAPIC;
	apic->lintr[0] = ApicIMASK;
	apic->lintr[1] = ApicIMASK;

	if(p->flags & PcmpBP){
		machno2apicno[0] = p->apicno;
		apic->machno = 0;
	}
	else{
		machno2apicno[conf.nmach] = p->apicno;
		apic->machno = conf.nmach;
		conf.nmach++;
	}

	return apic;
}

static Bus*
mkbus(PCMPbus* p)
{
	Bus *bus;
	int i;

	for(i = 0; buses[i]; i++){
		if(strncmp(buses[i], p->string, sizeof(p->string)) == 0)
			break;
	}
	if(buses[i] == 0)
		return 0;

	bus = xalloc(sizeof(Bus));
	if(mpbus)
		mpbuslast->next = bus;
	else
		mpbus = bus;
	mpbuslast = bus;

	bus->type = i;
	bus->busno = p->busno;
	if(bus->type == BusEISA){
		bus->po = PcmpLOW;
		bus->el = PcmpLEVEL;
	}
	else if(bus->type == BusPCI){
		bus->po = PcmpLOW;
		bus->el = PcmpLEVEL;
	}
	else{
		bus->po = PcmpHIGH;
		bus->el = PcmpEDGE;
	}

	return bus;
}

static Bus*
mpgetbus(int busno)
{
	Bus *bus;

	for(bus = mpbus; bus; bus = bus->next){
		if(bus->busno == busno)
			return bus;
	}
	print("mpgetbus: can't find bus %d\n", busno);

	return 0;
}

static Apic*
mkioapic(PCMPioapic* p)
{
	Apic *apic;
	ulong addr, *pte;

	if(!(p->flags & PcmpEN) || p->apicno > MaxAPICNO)
		return 0;

	/*
	 * Map the I/O APIC. This should be in the same 4MB segment
	 * as MACHADDR so no new 2nd level table will be allocated.
	 */
	addr = p->addr;
	if((pte = mmuwalk(m->pdb, addr, 1)) == 0)
		return 0;
	if(!(*pte & PTEVALID)){
		*pte = addr|PTEWRITE|PTEUNCACHED|PTEVALID;
		mmuflushtlb(PADDR(m->pdb));
	}

	apic = &mpapic[p->apicno];
	apic->type = PcmpIOAPIC;
	apic->apicno = p->apicno;
	apic->addr = KADDR(addr);
	apic->flags = p->flags;

	return apic;
}

static Aintr*
mkiointr(PCMPintr* p)
{
	Bus *bus;
	Aintr *aintr;

	/*
	 * According to the MultiProcessor Specification, a destination
	 * I/O APIC of 0xFF means the signal is routed to all I/O APICs.
	 * It's unclear how that can possibly be correct so treat it as
	 * an error for now.
	 */
	if(p->apicno == 0xFF)
		return 0;
	if((bus = mpgetbus(p->busno)) == 0)
		return 0;

	aintr = xalloc(sizeof(Aintr));
	aintr->intr = p;
	aintr->apic = &mpapic[p->apicno];
	aintr->next = bus->aintr;
	bus->aintr = aintr;

	return aintr;
}

static int
mpintrinit(Bus* bus, PCMPintr* intr, int vector)
{
	int el, po, v;

	/*
	 * Parse an I/O or Local APIC interrupt table entry and
	 * return the encoded vector.
	 */
	v = vector;

	po = intr->flags & PcmpPOMASK;
	el = intr->flags & PcmpELMASK;

	switch(intr->intr){

	default:				/* PcmpINT */
		v |= ApicLOWEST;
		break;

	case PcmpNMI:
		v |= ApicNMI;
		po = PcmpHIGH;
		el = PcmpEDGE;
		break;

	case PcmpSMI:
		v |= ApicSMI;
		break;

	case PcmpExtINT:
		v |= ApicExtINT;
		break;
	}

	/*
	 */
	if(bus->type == BusEISA && !po && !el /*&& !(elcr & (1<<(v-VectorPIC)))*/){
		po = PcmpHIGH;
		el = PcmpEDGE;
	}
	if(!po)
		po = bus->po;
	if(po == PcmpLOW)
		v |= ApicLOW;
	else if(po != PcmpHIGH){
		print("mpintrinit: bad polarity 0x%uX\n", po);
		return ApicIMASK;
	}

	if(!el)
		el = bus->el;
	if(el == PcmpLEVEL)
		v |= ApicLEVEL;
	else if(el != PcmpEDGE){
		print("mpintrinit: bad trigger 0x%uX\n", el);
		return ApicIMASK;
	}

	return v;
}

static int
mklintr(PCMPintr* p)
{
	Apic *apic;
	Bus *bus;
	int intin, v;

	/*
	 * The offsets of vectors for LINT[01] are known to be
	 * 0 and 1 from the local APIC vector space at VectorLAPIC.
	 * Can't use apic->vecbase here as this vector may be applied
	 * to all local APICs (apicno == 0xFF).
	 */
	if((bus = mpgetbus(p->busno)) == 0)
		return 0;
	intin = p->intin;
	v = mpintrinit(bus, p, VectorLAPIC+intin);

	if(p->apicno == 0xFF){
		for(apic = mpapic; apic <= &mpapic[MaxAPICNO]; apic++){
			if((apic->flags & PcmpEN) && apic->type == PcmpPROCESSOR)
				apic->lintr[intin] = v;
		}
	}
	else{
		apic = &mpapic[p->apicno];
		if((apic->flags & PcmpEN) && apic->type == PcmpPROCESSOR)
			apic->lintr[intin] = v;
	}

	return v;
}

#define PDX(va)		((((ulong)(va))>>22) & 0x03FF)
#define PTX(va)		((((ulong)(va))>>12) & 0x03FF)

static void
squidboy(Apic* apic)
{
	int clkin;

	/*iprint("Hello Squidboy\n");*/

	machinit();
	mmuinit();

	cpuidentify();
	cpuidprint();

	lock(&mprdthilock);
	mprdthi |= (1<<apic->apicno)<<24;
	unlock(&mprdthilock);

	/*
	 * Restrain your octopus! Don't let it go out on the sea!
	 */
	while(MACHP(0)->ticks == 0)
		;

	clkin = lapicinit(apic);
	lapiconline(clkin);

	lock(&active);
	active.machs |= 1<<m->machno;
	unlock(&active);

	schedinit();
}

static void
mpstartap(Apic* apic)
{
	ulong *apbootp, *pdb, *pte;
	Mach *mach;
	int i, machno;
	uchar *p;

	/*
	 * Initialise the AP page-tables and Mach structure. These are
	 * the same as for the bootstrap processor with the exception of
	 * the PTE for the Mach structure.
	 * Xspanalloc will panic if an allocation can't be made.
	 */
	pdb = xspanalloc(3*BY2PG, BY2PG, 0);
	memmove(pdb, (void*)CPU0PDB, BY2PG);

	pte = (ulong*)(((uchar*)pdb)+BY2PG);
	memmove(pte, (void*)CPU0MACHPTE, BY2PG);
	pdb[PDX(MACHADDR)] = PADDR(pte)|PTEWRITE|PTEVALID;

	mach = (Mach*)(((uchar*)pdb)+2*BY2PG);
	if((pte = mmuwalk(pdb, MACHADDR, 0)) == 0)
		return;
	*pte = PADDR(mach)|PTEWRITE|PTEVALID;

	machno = apic->machno;
	MACHP(machno) = mach;
	mach->machno = machno;
	mach->pdb = pdb;

	/*
	 * Tell the AP where its kernel vector and pdb are.
	 * The offsets are known in the AP bootstrap code.
	 */
	apbootp = (ulong*)(APBOOTSTRAP+0x08);
	*apbootp++ = (ulong)squidboy;
	*apbootp++ = PADDR(pdb);
	*apbootp = (ulong)apic;

	/*
	 * Universal Startup Algorithm.
	 */
	p = KADDR(0x467);
	*p++ = PADDR(APBOOTSTRAP);
	*p++ = PADDR(APBOOTSTRAP)>>8;
	i = (PADDR(APBOOTSTRAP) & ~0xFFFF)/16;
	*p++ = i;
	*p = i>>8;

	nvramwrite(0x0F, 0x0A);
	lapicstartap(apic, PADDR(APBOOTSTRAP));
	for(i = 0; i < 100000; i++){
		lock(&mprdthilock);
		if(mprdthi & ((1<<apic->apicno)<<24)){
			unlock(&mprdthilock);
			break;
		}
		unlock(&mprdthilock);
		microdelay(10);
	}
	nvramwrite(0x0F, 0x00);
}

void
mpinit(void)
{
	PCMP *pcmp;
	ulong *pte;
	uchar *e, *p;
	Apic *apic, *bpapic;
	int clkin;

	i8259init();

	if(_mp_ == 0)
		return;
	pcmp = KADDR(_mp_->physaddr);

	/*
	 * Map the local APIC. This should be in the same 4MB segment
	 * as MACHADDR so no new 2nd level table will be allocated.
	 */
	if((pte = mmuwalk(m->pdb, pcmp->lapicbase, 1)) == 0)
		return;
	if(!(*pte & PTEVALID)){
		*pte = pcmp->lapicbase|PTEWRITE|PTEUNCACHED|PTEVALID;
		mmuflushtlb(PADDR(m->pdb));
	}

	bpapic = 0;

	/*
	 * Run through the table saving information needed for starting application
	 * processors and initialising any I/O APICs. The table is guaranteed to be in
	 * order such that only one pass is necessary.
	 */
	p = ((uchar*)pcmp)+sizeof(PCMP);
	e = ((uchar*)pcmp)+pcmp->length;
	while(p < e) switch(*p){

	case PcmpPROCESSOR:
		if(apic = mkprocessor((PCMPprocessor*)p)){
			/*
			 * Must take a note of bootstrap processor APIC
			 * now as it will be needed in order to start the
			 * application processors later and there's no
			 * guarantee that the bootstrap processor appears
			 * first in the table before the others.
			 */
			apic->addr = KADDR(pcmp->lapicbase);
			if(apic->flags & PcmpBP)
				bpapic = apic;
		}
		p += sizeof(PCMPprocessor);
		continue;

	case PcmpBUS:
		mkbus((PCMPbus*)p);
		p += sizeof(PCMPbus);
		continue;

	case PcmpIOAPIC:
		if(apic = mkioapic((PCMPioapic*)p))
			ioapicinit(apic, ((PCMPioapic*)p)->apicno);
		p += sizeof(PCMPioapic);
		continue;

	case PcmpIOINTR:
		mkiointr((PCMPintr*)p);
		p += sizeof(PCMPintr);
		continue;

	case PcmpLINTR:
		mklintr((PCMPintr*)p);
		p += sizeof(PCMPintr);
		continue;
	}

	/*
	 * No bootstrap processor, no need to go further.
	 */
	if(bpapic == 0)
		return;

	clkin = lapicinit(bpapic);
	lock(&mprdthilock);
	mprdthi |= (1<<bpapic->apicno)<<24;
	unlock(&mprdthilock);

	/*
	 * These interrupts are local to the processor
	 * and do not appear in the I/O APIC so it is OK
	 * to set them now.
	 */
	intrenable(VectorTIMER, clockintr, 0, BUSUNKNOWN);
	intrenable(VectorERROR, lapicerror, 0, BUSUNKNOWN);
	intrenable(VectorSPURIOUS, lapicspurious, 0, BUSUNKNOWN);
	lapiconline(clkin);

	/*
	 * Initialise the application processors.
	 */
	memmove((void*)APBOOTSTRAP, apbootstrap, sizeof(apbootstrap));
	for(apic = mpapic; apic <= &mpapic[MaxAPICNO]; apic++){
		if((apic->flags & (PcmpBP|PcmpEN)) == PcmpEN && apic->type == PcmpPROCESSOR)
			mpstartap(apic);
	}

	/*
	 * Remember to set conf.copymode here if nmach > 1.
	 * Look for an ExtINT line and enable it.
	 */
	if(conf.nmach > 1)
		conf.copymode = 1;
}

static int
mpintrenablex(int v, int tbdf, Irqctl* irqctl)
{
	Bus *bus;
	Aintr *aintr;
	Apic *apic;
	int bno, dno, lo, n, type;

	if(v >= VectorLAPIC && v <= MaxVectorLAPIC){
		if(v != VectorSPURIOUS)
			irqctl->isr = lapiceoi;
		irqctl->isintr = 1;
		return v;
	}

	if(v < VectorPIC || v > MaxVectorPIC)
		return 0;

	/*
	 * Find the bus, default is ISA.
	 * There cannot be multiple ISA or EISA buses.
	 */
	if(tbdf == BUSUNKNOWN){
		type = BusISA;
		bno = 0;
		dno = -1;
	}
	else{
		type = BUSTYPE(tbdf);
		bno = BUSBNO(tbdf);
		dno = BUSDNO(tbdf);
	}
	n = 0;
	for(bus = mpbus; bus; bus = bus->next){
		if(bus->type != type)
			continue;
		if(n == bno)
			break;
		n++;
	}
	if(bus == 0){
		/*
		 * The MP configuration table on some older systems
		 * (e.g. ASUS PCI/E-P54NP4) has an entry for the EISA bus
		 * but none for ISA. It also has the interrupt type and
		 * polarity set to 'default for this bus' which wouldn't
		 * be compatible with ISA.
		 */
		for(bus = mpbus; bus; bus = bus->next){
			if(bus->type == BusEISA)
				break;
		}
		if(bus == 0){
			print("ioapicirq: can't find bus type %d\n", type);
			return -1;
		}
	}

	/*
	 * Find the interrupt info.
	 */
	for(aintr = bus->aintr; aintr; aintr = aintr->next){
		if(bus->type == BusPCI){
			n = (aintr->intr->irq>>2) & 0x1F;
			if(n != dno)
				continue;
		}
		else{
			n = aintr->intr->irq;
			if(n != (v-VectorPIC))
				continue;
		}

		lo = mpintrinit(bus, aintr->intr, v);
		if(lo & ApicIMASK)
			return -1;
		lo |= ApicLOGICAL;

		apic = aintr->apic;
		if((apic->flags & PcmpEN) && apic->type == PcmpIOAPIC){
			lock(&mprdthilock);
 			ioapicrdtw(apic, aintr->intr->intin, mprdthi, lo);
			unlock(&mprdthilock);
		}

		irqctl->isr = lapicisr;
		irqctl->eoi = lapiceoi;
		irqctl->isintr = 1;

		return lo & 0xFF;
	}

	return -1;
}

int
mpintrenable(int v, int tbdf, Irqctl* irqctl)
{
	int r;

	r = mpintrenablex(v, tbdf, irqctl);
	if(r == -1 && tbdf != BUSUNKNOWN && _mp_->specrev == 1)
		return mpintrenablex(v, BUSUNKNOWN, irqctl);
	return r;
}

void
mpshutdown(void)
{
	/*
	 * To be done...
	 */
	if(m->machno){
		/*
		 * If this processor received the CTRL-ALT-DEL from
		 * the keyboard, acknowledge it. Send an INIT to self.
		 */
		if(lapicisr(VectorKBD))
			lapiceoi(VectorKBD);
		lapicicrw(0, 0x00040000|ApicINIT);	
		idle();
	}

	print("apshutdown: active = 0x%2.2uX\n", active.machs);
	delay(1000);
	splhi();

	/*
	 * INIT all excluding self.
	 */
	lapicicrw(0, 0x000C0000|ApicINIT);

#ifdef notdef
	/*
	 * Often the BIOS hangs during restart if a conventional 8042
	 * warm-boot sequence is tried. The following is Intel specific and
	 * seems to perform a cold-boot, but at least it comes back.
	 */
	*(ushort*)(KZERO|0x472) = 0x1234;		/* BIOS warm-boot flag */
	outb(0xCF9, 0x02);
	outb(0xCF9, 0x06);
#else
	pcireset();
	i8042reset();
#endif /* notdef */
}
