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
	SRX=		0x3C4,		/* index to sequence registers */
	SR=		0x3C5,		/* sequence registers 0-7 */
};

void
srout(int reg, int val)
{
	outb(SRX, reg);
	outb(SR, val);
}

void
grout(int reg, int val)
{
	outb(GRX, reg);
	outb(GR, val);
}

/*
 *  look at VGA registers
 */
void
vgainit(void)
{
	uchar *display;
	int i;

	srout(2, 0x0f);		/* enable all 4 color planes */
	srout(4, 0x08);		/* quad mode */
	grout(5, 0x40);		/* pixel bits are sequential */
	grout(6, 0x01);		/* graphics mode - display starts at 0xA0000 */

	for(;;);
	display = (uchar*)(0xA0000 | KZERO);
	for(i = 0; i < 128*1024; i++)
		display[i] = 0x00;

	for(i = 0; i < 4*640; i++)
		display[i] = 0xff;
}
