#include	"mem.h"

/* use of SPRG registers in save/restore */
#define	SAVER0	SPRG0
#define	SAVER1	SPRG1
#define	SAVELR	SPRG2
#define	SAVEXX	SPRG3

/* special instruction definitions */
#define	BDNZ	BC	16,0,
#define	BDNE	BC	0,2,
//#define	TLBIA	WORD	$(31<<26)
#define	TLBIA	WORD	$((31<<26)|(370<<1))
//#define	MFTB(tbr,d)	WORD	$((31<<26)|((d)<<21)|((tbr&0x1f)<<16)|(((tbr>>5)&0x1f)<<11)|(371<<1))

/* on some models mtmsr doesn't synchronise enough (eg, 603e) */
#define	MSRSYNC	SYNC; ISYNC

#define	UREGSPACE	(UREGSIZE+8)

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
	MSRSYNC
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
	MSRSYNC
	RETURN

TEXT	spllo(SB), $0
	MOVW	MSR, R3
	OR	$MSR_EE, R3, R4
	SYNC
	MOVW	R4, MSR
	MSRSYNC
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

TEXT	touser(SB), $-4
	MOVW	$(UTZERO+32), R5	/* header appears in text */
	MOVW	$(MSR_EE|MSR_PR|MSR_ME|MSR_IR|MSR_DR|MSR_RI), R4
	MOVW	R4, SPR(SRR1)
	MOVW	R3, R1
	MOVW	R5, SPR(SRR0)
	RFI

TEXT	icflush(SB), $-4	/* icflush(virtaddr, count) */
	MOVW	n+4(FP), R4
	RLWNM	$0, R3, $~(CACHELINESZ-1), R5
	SUB	R5, R3
	ADD	R3, R4
	ADD		$(CACHELINESZ-1), R4
	SRAW	$CACHELINELOG, R4
	MOVW	R4, CTR
icf0:	ICBI	(R5)
	ADD	$CACHELINESZ, R5
	BDNZ	icf0
	ISYNC
	RETURN

TEXT	dcflush(SB), $-4	/* dcflush(virtaddr, count) */
	MOVW	n+4(FP), R4
	RLWNM	$0, R3, $~(CACHELINESZ-1), R5
	CMP	R4, $0
	BLE	dcf1
	SUB	R5, R3
	ADD	R3, R4
	ADD		$(CACHELINESZ-1), R4
	SRAW	$CACHELINELOG, R4
	MOVW	R4, CTR
dcf0:	DCBF	(R5)
	ADD	$CACHELINESZ, R5
	BDNZ	dcf0
dcf1:
	RETURN

TEXT	tas(SB), $0
	SYNC
	MOVW	R3, R4
	MOVW	$0xdead,R5
tas1:
	DCBF	(R4)	/* fix for 603x bug */
	LWAR	(R4), R3
	CMP	R3, $0
	BNE	tas0
	STWCCC	R5, (R4)
	BNE	tas1
tas0:
	SYNC
	ISYNC
	RETURN

TEXT	getpvr(SB), $0
	MOVW	SPR(PVR), R3
	RETURN

TEXT	getdec(SB), $0
	MOVW	SPR(DEC), R3
	RETURN

TEXT	putdec(SB), $0
	MOVW	R3, SPR(DEC)
	RETURN

TEXT	getdar(SB), $0
	MOVW	SPR(DAR), R3
	RETURN

TEXT	getdsisr(SB), $0
	MOVW	SPR(DSISR), R3
	RETURN

TEXT	getmsr(SB), $0
	MOVW	MSR, R3
	RETURN

TEXT	putmsr(SB), $0
	SYNC
	MOVW	R3, MSR
	MSRSYNC
	RETURN

TEXT	eieio(SB), $0
	EIEIO
	RETURN

TEXT	tlbflushall(SB), $0
	TLBIA
	RETURN

TEXT	tlbflush(SB), $0
	TLBIE	R3
	RETURN

TEXT	gotopc(SB), $0
	MOVW	R3, CTR
	MOVW	LR, R31	/* for trace back */
	BR	(CTR)

/*
 * traps force memory mapping off.
 * the following code has been executed at the exception
 * vector location
 *	MOVW R0, SPR(SAVER0)
 *	MOVW LR, R0
 *	MOVW R0, SPR(SAVELR) 
 *	bl	trapvec(SB)
 */
TEXT	trapvec(SB), $-4
	MOVW	LR, R0
	MOVW	R1, SPR(SAVER1)
	MOVW	R0, SPR(SAVEXX)	/* vector */

/*
	to enable hardware break points
	MOVW	MSR, R1
	OR		$(MSR_RI), R1
	MOVW	R1, MSR
	ISYNC
*/	

	/* did we come from user space */
	MOVW	SPR(SRR1), R0
	MOVW	CR, R1
	MOVW	R0, CR
	BC	4,17,ktrap
	
	/* switch to kernel stack */
	MOVW	R1, CR
	MOVW	$(MACHADDR&~KZERO), R1	/* PADDR(m->) */
	MOVW	12(R1), R1				/* m->proc  */
	RLWNM	$0, R1, $~KZERO, R1		/* PADDR(m->proc) */
	MOVW	8(R1), R1				/* m->proc->kstack */
	RLWNM	$0, R1, $~KZERO, R1		/* PADDR(m->proc->kstack) */
	ADD	$(KSTACK-UREGSIZE), R1
	BL	saveureg(SB)
	BL	trap(SB)
	BR	restoreureg
ktrap:
	MOVW	R1, CR
	MOVW	SPR(SAVER1), R1
	RLWNM	$0, R1, $~KZERO, R1		/* PADDR(m->proc->kstack) */
	SUB	$UREGSPACE, R1
	BL	saveureg(SB)
	BL	trap(SB)
	BR	restoreureg


/*
 * enter with stack set and mapped.
 * on return, SB (R2) has been set, and R3 has the Ureg*,
 * the MMU has been re-enabled, kernel text and PC are in KSEG,
 * R(MACH) has been set, and R0 contains 0.
 *
 */
TEXT	saveureg(SB), $-4
/*
 * save state
 */
	MOVMW	R2, 48(R1)	/* r2:r31 */
	MOVW	$setSB(SB), R2
	MOVW	$(MACHADDR&~KZERO), R(MACH)
	MOVW	12(R(MACH)), R(USER)
	MOVW	$MACHADDR, R(MACH)
	MOVW	SPR(SAVER1), R4
	MOVW	R4, 44(R1)
	MOVW	SPR(SAVER0), R5
	MOVW	R5, 40(R1)
	MOVW	CTR, R6
	MOVW	R6, 36(R1)
	MOVW	XER, R4
	MOVW	R4, 32(R1)
	MOVW	CR, R5
	MOVW	R5, 28(R1)
	MOVW	SPR(SAVELR), R6	/* LR */
	MOVW	R6, 24(R1)
	/* pad at 20(R1) */
	MOVW	SPR(SRR0), R0
	MOVW	R0, 16(R1)				/* old PC */
	MOVW	SPR(SRR1), R0
	MOVW	R0, 12(R1)				/* old status */
	MOVW	SPR(SAVEXX), R0
	MOVW	R0, 8(R1)	/* cause/vector */
	ADD	$8, R1, R3	/* Ureg* */
	OR	$KZERO, R3	/* fix ureg */
	STWCCC	R3, (R1)	/* break any pending reservations */
	MOVW	$0, R0	/* compiler/linker expect R0 to be zero */

	MOVW	MSR, R5
//	OR	$(MSR_IR|MSR_DR|MSR_RI), R5	/* enable MMU */
	MOVW	R5, SPR(SRR1)
	MOVW	LR, R31
//	OR	$KZERO, R31	/* return PC in KSEG0 */
	MOVW	R31, SPR(SRR0)
//	OR	$KZERO, R1	/* fix stack pointer */
	RFI	/* returns to trap handler */

/*
 * restore state from Ureg and return from trap/interrupt
 */
TEXT	forkret(SB), $0
	BR	restoreureg

restoreureg:
	MOVMW	48(R1), R2	/* r2:r31 */
	/* defer R1 */
	MOVW	40(R1), R0
	MOVW	R0, SPR(SAVER0)
	MOVW	36(R1), R0
	MOVW	R0, CTR
	MOVW	32(R1), R0
	MOVW	R0, XER
	MOVW	28(R1), R0
	MOVW	R0, CR	/* CR */
	MOVW	24(R1), R0
	MOVW	R0, LR
	/* pad, skip */
	MOVW	16(R1), R0
	MOVW	R0, SPR(SRR0)	/* old PC */
	MOVW	12(R1), R0
	MOVW	R0, SPR(SRR1)	/* old MSR */
	/* cause, skip */
	MOVW	44(R1), R1	/* old SP */
	MOVW	SPR(SAVER0), R0
	RFI
