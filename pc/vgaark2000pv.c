#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include <libg.h>
#include "screen.h"
#include "vga.h"

static void
ark2000pvpage(int page)
{
	vgaxo(Seqx, 0x15, page);
	vgaxo(Seqx, 0x16, page);
}

static Vgac ark2000pv = {
	"ark2000pv",
	ark2000pvpage,

	0,
};

void
vgaark2000pvlink(void)
{
	addvgaclink(&ark2000pv);
}
