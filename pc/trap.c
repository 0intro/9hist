#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"

/*
 *  trap/interrupt gates
 */
Segdesc ilt[256];
void	(*ivec[256])(void*);

void
sethvec(int v, void (*r)(void), int type)
{
	ilt[v].d0 = ((ulong)r)&0xFFFF|(KESEL<<16);
	ilt[v].d1 = ((ulong)r)&0xFFFF0000|SEGP|SEGPL(3)|type;
}

void
setvec(int v, void (*r)(Ureg*), int type)
{
	ilt[v].d1 &= ~SEGTYPE;
	ilt[v].d1 |= type;
	ivec[v] = r;
}

/*
 *  set up the interrupt/trap gates
 */
void
trapinit(void)
{
	int i;

	/*
	 *  set the standard traps
	 */
	sethvec(0, intr0, SEGTG);
	sethvec(1, intr1, SEGTG);
	sethvec(2, intr2, SEGTG);
	sethvec(3, intr3, SEGTG);
	sethvec(4, intr4, SEGTG);
	sethvec(5, intr5, SEGTG);
	sethvec(6, intr6, SEGTG);
	sethvec(7, intr7, SEGTG);
	sethvec(8, intr8, SEGTG);
	sethvec(9, intr9, SEGTG);
	sethvec(10, intr10, SEGTG);
	sethvec(11, intr11, SEGTG);
	sethvec(12, intr12, SEGTG);
	sethvec(13, intr13, SEGTG);
	sethvec(14, intr14, SEGTG);
	sethvec(15, intr15, SEGTG);
	sethvec(16, intr16, SEGTG);
	sethvec(17, intr17, SEGTG);
	sethvec(18, intr18, SEGTG);
	sethvec(19, intr19, SEGTG);
	sethvec(20, intr20, SEGTG);
	sethvec(21, intr21, SEGTG);
	sethvec(22, intr22, SEGTG);
	sethvec(23, intr23, SEGTG);

	/*
	 *  set all others to unknown
	 */
	for(i = 24; i < 256; i++)
		sethvec(i, intrbad, SEGIG);

	/*
	 *  tell the hardware where the table is (and how long)
	 */
	lidt(ilt, sizeof(ilt));

	/*
	 *  Set up the 8259 interrupt processor #1.
	 *  Make 8259 interrupts starting at vector I8259vec.
	 *
	 *  N.B. This is all magic to me.  It comes from the 
	 *	 IBM technical reference manual.  I just
	 *	 changed the vector.
	 */
	outb(Int0ctl, 0x11);		/* ICW1 - edge, master, ICW4 */
	outb(Int0aux, Int0vec);		/* ICW2 - interrupt vector */
	outb(Int0aux, 0x04);		/* ICW3 - master level 2 */
	outb(Int0aux, 0x01);		/* ICW4 - master, 8086 mode */
	outb(Int0aux, 0x00);		/* mask - all enabled */
}


/*
 *  All traps
 */
trap(Ureg *ur)
{
	if(ur->trap>=256 || ivec[ur->trap] == 0)
		panic("bad trap type %d\n", ur->trap);

	(*ivec[ur->trap])(ur);
	INT0ENABLE;
}
