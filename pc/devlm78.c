#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

// this driver assumes that noone has changed the serial address
// of the device.  if they have, there's no way we can figure it
// out -- presotto

enum
{
	// address of chip on serial interface
	Serialaddr=	0x2d,

	// internal register addresses
	Rconfig=	0x40,
	Ristat1=	0x41,
	Ristat2=	0x42,
	Rsmimask1=	0x43,
	Rsmimask2=	0x44,
	Rnmimask1=	0x45,
	Rnmimask2=	0x46,
	Rvidfan=	0x47,		// set fan counter, and read voltage level
	 Mvid=		 0x0f,
	 Mfan=		 0xf0,
	Raddr=		0x48,		// address used on serial bus
	Rresetid=	0x49,		// chip reset and ID register
	Rpost=		0x00,		// start of post ram
	Rvalue=		0x20,		// start of value ram

	VRsize=		0x20,		// size of value ram
};

enum
{
	Qdir,
	Qlm78vram,
};

static Dirtab lm78dir[] = {
	"lm78vram",		{ Qlm78vram, 0 },		0,	0444,
};

// interface type
enum
{
	None=	0,
	Smbus,
	Parallel,
};

static struct {
	QLock;

	int 	ifc;

	// serial interface
	SMBus	*smbus;

	// parallel interface
	int	port;
} lm78;

extern SMBus*	piix4smbus(void);

// routines that actually touch the device
static int
lm78wrreg(int reg, uchar val)
{
	switch(lm78.ifc){
	case Smbus:
		lm78.smbus->transact(lm78.smbus, SMBbytewrite, Serialaddr, reg, &val);
		return 0;
	}
	return -1;
}

static int
lm78rdreg(int reg)
{
	uchar rv;

	switch(lm78.ifc){
	case Smbus:
		lm78.smbus->transact(lm78.smbus, SMBsend, Serialaddr, reg, nil);
		lm78.smbus->transact(lm78.smbus, SMBrecv, Serialaddr, 0, &rv);
		return rv;
	}
	return -1;
}

static int
lm78probe(void)
{
	switch(lm78.ifc){
	case Smbus:
		if(lm78rdreg(Raddr) != Serialaddr){
			lm78.ifc = None;
			break;
		}
		return 0;
	}
	return -1;
}

enum
{
	IntelVendID=	0x8086,
	PiixID=		0x122E,
	Piix3ID=	0x7000,

	Piix4PMID=	0x7113,		// PIIX4 power management function

	PCSC=		0x78,		// programmable chip select control register
	PCSC8bytes=	0x01,
};

// figure out what kind of interface and if there's an lm78 there
void
lm78reset(void)
{
	int pcs;
	Pcidev *p;

	lm78.ifc = None;
	p = nil;
	while((p = pcimatch(p, IntelVendID, 0)) != nil){
		switch(p->did){
		// these bridges use the PCSC to map the lm78 into port space.
		// for this case the lm78's CS# select is connected to the PIIX's
		// PCS# output and the bottom 3 bits of address are passed to the
		// LM78's A0-A2 inputs.
		case PiixID:
		case Piix3ID:
			pcs = pcicfgr16(p, PCSC);
			if(pcs & 3) {
				/* already enabled */
				lm78.port = pcs & ~3;
				lm78.ifc = Parallel;
				return;	
			}

			// enable the chip, use default address 0x50
			pcicfgw16(p, PCSC, 0x50|PCSC8bytes);
			pcs = pcicfgr16(p, PCSC);
			lm78.port = pcs & ~3;
			lm78.ifc = Parallel;
			return;

		// this bridge puts the lm78's serial interface on the smbus
		case Piix4PMID:
			lm78.smbus = piix4smbus();
			if(lm78.smbus == nil)
				continue;
			print("found piix4 smbus, base %lud\n", lm78.smbus->base);
			lm78.ifc = Smbus;
			return;
		}
	}
}

static Chan*
lm78attach(char* spec)
{
	static int probed;

	if(lm78.ifc == None)
		error(Enodev);

	if(probed == 0){
		if(lm78probe() < 0)
			error(Enodev);
		probed = 1;
	}
	return devattach('T', spec);
}

int
lm78walk(Chan* c, char* name)
{
	return devwalk(c, name, lm78dir, nelem(lm78dir), devgen);
}

static void
lm78stat(Chan* c, char* dp)
{
	devstat(c, dp, lm78dir, nelem(lm78dir), devgen);
}

static Chan*
lm78open(Chan* c, int omode)
{
	return devopen(c, omode, lm78dir, nelem(lm78dir), devgen);
}

static void
lm78close(Chan*)
{
}

enum
{
	Linelen= 25,
};

static long
lm78read(Chan *c, void *a, long n, vlong offset)
{
	uchar *va = a;
	int off, e;

	off = offset;

	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, a, n, lm78dir, nelem(lm78dir), devgen);

	case Qlm78vram:
		if(off >=  VRsize)
			return 0;
		if(waserror()){
			qunlock(&lm78);
			nexterror();
		}
		qlock(&lm78);
		e = off + n;
		if(e > VRsize)
			e = VRsize;
		for(; off < e; off++)
			*va++ = lm78rdreg(off);
		qunlock(&lm78);
		poperror();
		return va - (uchar*)a;
	}
	return 0;
}

static long
lm78write(Chan *c, void *a, long n, vlong offset)
{
	uchar *va = a;
	int off, e;

	off = offset;

	switch(c->qid.path){
	default:
		error(Eperm);

	case Qlm78vram:
		if(off >=  VRsize)
			return 0;
		if(waserror()){
			qunlock(&lm78);
			nexterror();
		}
		qlock(&lm78);
		e = off + n;
		if(e > VRsize)
			e = VRsize;
		for(; off < e; off++)
			lm78wrreg(off, *va++);
		qunlock(&lm78);
		poperror();
		return va - (uchar*)a;
	}
	return 0;
}

Dev lm78devtab = {
	'T',
	"lm78",

	lm78reset,
	devinit,
	lm78attach,
	devclone,
	lm78walk,
	lm78stat,
	lm78open,
	devcreate,
	lm78close,
	lm78read,
	devbread,
	lm78write,
	devbwrite,
	devremove,
	devwstat,
};

