#include "mem.h"

TEXT	start(SB), $-4

	/* get virtual, fast */
	/* we are executing in segment 0, mapped to pmeg 0. stack is there too */
	/* get virtual by mapping segment(KZERO) to pmeg 0. */
	MOVW	$KZERO, R1
	MOVB	R0, (R1, 3)
	/* now mapped correctly.  jmpl to where we want to be */
	MOVW	$setR30(SB), R30
	MOVW	$startvirt(SB), R1
	JMPL	(R1)
	RETURN			/* can't get here */

TEXT	startvirt(SB), $-4

	MOVW	$0x4000, R29
	MOVW	$mach0(SB), R(MACH)
	JMPL	main(SB)
	UNIMP
	RETURN

TEXT	pc(SB), $0
	MOVW	R15, R1
	RETURN

TEXT	setlabel(SB), $0
	MOVW	b+0(FP), R2
	MOVW	R29, (R2)
	MOVW	R15, 4(R2)
	MOVW	$0, R1
	RETURN

TEXT	gotolabel(SB), $0
	MOVW	r+4(FP), R1
	MOVW	b+0(FP), R2
	MOVW	(R2), R29
	MOVW	4(R2), R15
	MOVW	R15, 0(R29)
	RETURN

TEXT	crash(SB), $0

	MOVW	0(FP), R8		/* context */
	MOVW	4(FP), R9		/* segment addr */
	MOVW	8(FP), R10		/* segment value */
	MOVW	$0xFFE80118, R1
	JMPL	(R1)
	RETURN

TEXT	putb2(SB), $0

	MOVW	0(FP), R1
	MOVW	4(FP), R2
	MOVB	R2, (R1, 2)
	RETURN

TEXT	putw3(SB), $0

	MOVW	0(FP), R1
	MOVW	4(FP), R2
	MOVW	R2, (R1, 3)
	RETURN

TEXT	putpmeg(SB), $0

	MOVW	0(FP), R1
	MOVW	4(FP), R2
	MOVW	R2, (R1, 4)
	RETURN

TEXT	putwd(SB), $0

	MOVW	0(FP), R1
	MOVW	4(FP), R2
	MOVW	R2, (R1, 0xD)
	RETURN


GLOBL	mach0+0(SB), $MACHSIZE
