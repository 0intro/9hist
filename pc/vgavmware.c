#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

enum {
	PCIVMWARE	= 0x15AD,	/* PCI VID */

	VMWARE1		= 0x0710,	/* PCI DID */
	VMWARE2		= 0x0405,
};

enum {
	Rid = 0,
	Renable,
	Rwidth,
	Rheight,
	Rmaxwidth,
	Rmaxheight,
	Rdepth,
	Rbpp,
	Rpseudocolor,
	Rrmask,
	Rgmask,
	Rbmask,
	Rbpl,
	Rfbstart,
	Rfboffset,
	Rfbmaxsize,
	Rfbsize,
	Rcap,
	Rmemstart,
	Rmemsize,
	Rconfigdone,
	Rsync,
	Rbusy,
	Rguestid,
	Rcursorid,
	Rcursorx,
	Rcursory,
	Rcursoron,
	Nreg,

	Rpalette = 1024,
};

typedef struct Vmware	Vmware;
struct Vmware {
	ulong	mmio;
	ulong	fb;

	ulong	ra;
	ulong	rd;

	ulong	r[Nreg];

	char	chan[32];
	int	depth;
};

Vmware xvm;
Vmware *vm=&xvm;

static ulong
vmrd(Vmware *vm, int i)
{
	outl(vm->ra, i);
	return inl(vm->rd);
}

static void
vmwr(Vmware *vm, int i, ulong v)
{
	outl(vm->ra, i);
	outl(vm->rd, v);
}

static ulong
vmwarelinear(VGAscr* scr, int* size, int* align)
{
	char err[64];
	ulong aperture, oaperture;
	int osize, oapsize, wasupamem;
	Pcidev *p;
	Physseg seg;

	osize = *size;
	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;

	p = pcimatch(nil, PCIVMWARE, 0);
	if(p == nil)
		error("no vmware card found");

	switch(p->did){
	default:
		snprint(err, sizeof err, "unknown vmware id %.4ux", p->did);
		error(err);
		
	case VMWARE1:
		vm->ra = 0x4560;
		vm->rd = 0x4560+4;
		break;

	case VMWARE2:
		vm->ra = p->mem[0].bar&~3;
		vm->rd = vm->ra + 1;
	}

	aperture = (ulong)(vmrd(vm, Rfbstart));
	*size = vmrd(vm, Rfbsize);

	if(wasupamem)
		upafree(oaperture, oapsize);
	scr->isupamem = 0;

	aperture = upamalloc(aperture, *size, *align);
	if(aperture == 0){
		if(wasupamem && upamalloc(oaperture, oapsize, 0))
			scr->isupamem = 1;
	}else
		scr->isupamem = 1;

	if(oaperture)
		print("warning (BUG): redefinition of aperture does not change vmwarescreen segment\n");
	memset(&seg, 0, sizeof(seg));
	seg.attr = SG_PHYSICAL;
	seg.name = smalloc(32);
	snprint(seg.name, 32, "vmwarescreen");
	seg.pa = aperture;
	seg.size = osize;
	addphysseg(&seg);
	return aperture;
}

static void
vmwaredisable(VGAscr*)
{
}

static void
vmwareload(VGAscr*, Cursor*)
{
}

static int
vmwaremove(VGAscr*, Point)
{
	return 0;
}

static void
vmwareenable(VGAscr*)
{
}

static void
vmwareblank(int)
{
}

static void
vmwaredrawinit(VGAscr*)
{
}

VGAdev vgavmwaredev = {
	"vmware",

	0,
	0,
	0,
	vmwarelinear,
	vmwaredrawinit,
};

VGAcur vgavmwarecur = {
	"vmwarehwgc",

	vmwareenable,
	vmwaredisable,
	vmwareload,
	vmwaremove,
};
