#include	"mem.h"

#define	BDNZ	BC	16,0,
#define	BDNE	BC	0,2,
#define	NOOP	OR	R0,R0,R0
#define	TLBIA	WORD	$(31<<26)

/* Be-ware 603e chip bugs (mtmsr instruction) */
#define	FIX603e	CROR	0,0,0
#undef FIX603e
#define	FIX603e	ISYNC; SYNC

	TEXT start(SB), $-4
	MOVW	$setSB(SB), R2

	BL	main(SB)
	RETURN		/* not reached */

TEXT	splhi(SB), $0
	MOVW	LR, R31
	MOVW	R31, 4(R(MACH))	/* save PC in m->splpc */
	MOVW	MSR, R3
	RLWNM	$0, R3, $~MSR_EE, R4
	SYNC
	MOVW	R4, MSR
	FIX603e
	RETURN

TEXT	splx(SB), $0
	/* fall though */

TEXT	splxpc(SB), $0
	MOVW	LR, R31
	MOVW	R31, 4(R(MACH))	/* save PC in m->splpc */
	MOVW	MSR, R4
	RLWMI	$0, R3, $MSR_EE, R4
	SYNC
	MOVW	R4, MSR
	FIX603e
	RETURN

TEXT	spllo(SB), $0
	MOVW	MSR, R3
	OR	$MSR_EE, R3, R4
	SYNC
	MOVW	R4, MSR
	FIX603e
	RETURN

TEXT	spldone(SB), $0
	RETURN

TEXT	islo(SB), $0
	MOVW	MSR, R3
	RLWNM	$0, R3, $MSR_EE, R3
	RETURN

TEXT	setlabel(SB), $-4
	MOVW	LR, R31
	MOVW	R1, 0(R3)
	MOVW	R31, 4(R3)
	MOVW	$0, R3
	RETURN

TEXT	gotolabel(SB), $-4
	MOVW	4(R3), R31
	MOVW	R31, LR
	MOVW	0(R3), R1
	MOVW	$1, R3
	RETURN
