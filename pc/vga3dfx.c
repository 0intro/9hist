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

static ulong
tdfxlinear(VGAscr* scr, int* size, int* align)
{
	ulong aperture, oaperture;
	int oapsize, wasupamem;
	Pcidev *p;

	oaperture = scr->aperture;
	oapsize = scr->apsize;
	wasupamem = scr->isupamem;

	aperture = 0;
	if(p = pcimatch(nil, 0x121A, 0)){
		switch(p->did){
		case 0x0005:		/* Voodoo 3 3000 */
			aperture = p->mem[1].bar & ~0x0F;
			*size = p->mem[1].size;
			break;
		default:
			break;
		}
	}

	if(wasupamem){
		if(oaperture == aperture)
			return oaperture;
		upafree(oaperture, oapsize);
	}
	scr->isupamem = 0;

	aperture = upamalloc(aperture, *size, *align);
	if(aperture == 0){
		if(wasupamem && upamalloc(oaperture, oapsize, 0)){
			aperture = oaperture;
			scr->isupamem = 1;
		}
		else
			scr->isupamem = 0;
	}
	else
		scr->isupamem = 1;

	return aperture;
}

VGAdev vga3dfxdev = {
	"3dfx",

	nil,				/* enable */
	nil,				/* disable */
	nil,				/* page */
	tdfxlinear,			/* linear */
	nil,				/* drawinit */
	nil,				/* fill */
};

VGAcur vga3dfxcur = {
	"3dfxhwgc",

	nil,				/* enable */
	nil,				/* disable */
	nil,				/* load */
	nil,				/* move */
};
