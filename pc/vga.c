#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

enum
{
	GRX=		0x3CE,		/* index to graphics registers */
	GR=		0x3CF,		/* graphics registers 0-8 */
};

/*
 *  look at VGA registers
 */
void
vgainit(void)
{
	int i;
	uchar x;

	for(i = 0; i < 9; i++){
		outb(GRX, i);
		x = inb(GR);
		print("GR%d == %ux\n", i, x);
	}
	panic("for the hell of it");
}
