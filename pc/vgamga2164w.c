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
#include "screen.h"

static void
mga2164wenable(VGAscr*)
{
	Pcidev *p;
	Physseg *s;
/*
	ulong mmio;
	uchar *rp;
	int i;
*/

	if((p = pcimatch(nil, 0x102B, 0x051B)) == nil)
		return;

	for(s = physseg; s->name; s++)
		if(strcmp("pcivctl", s->name) == 0)
			s->pa = p->mem[1].bar & ~0x0F;

/*
	mmio = p->mem[1].bar & ~0x0F;
	mmio = upamalloc(mmio, 16*1024, 0);
	if(mmio == 0){
		print("mmio == 0\n");
		return;
	}

	rp = (uchar*)(mmio+0x3C00);
	for(i = 0; i < 16; i++){
		print("%2.2uX ", *rp);
		rp++;
	}
	print("\n");
*/
}

static ulong
mga2164wlinear(VGAscr* scr, int* size, int* align)
{
	ulong aperture, oaperture;
	int oapsize, wasupamem;
	Pcidev *p;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;
	if(wasupamem)
		upafree(oaperture, oapsize);
	scr->isupamem = 0;

	if(p = pcimatch(nil, 0x102B, 0x051B)){
		aperture = p->mem[0].bar & ~0x0F;
		*size = p->mem[0].size;
	}
	else
		aperture = 0;

	aperture = upamalloc(aperture, *size, *align);
	if(aperture == 0){
		if(wasupamem && upamalloc(oaperture, oapsize, 0))
			scr->isupamem = 1;
	}
	else
		scr->isupamem = 1;

	return aperture;
}

VGAdev vgamga2164wdev = {
	"mga2164w",

	mga2164wenable,			/* enable */
	0,				/* disable */
	0,				/* page */
	mga2164wlinear,			/* linear */
};
