#include	"mem.h"

/*
 * common ppc special purpose registers
 */
#define DSISR	18
#define DAR	19	/* Data Address Register */
#define DEC	22	/* Decrementer */
#define SRR0	26	/* Saved Registers (exception) */
#define SRR1	27
#define SPRG0	272	/* Supervisor Private Registers */
#define SPRG1	273
#define SPRG2	274
#define SPRG3	275
#define TBRU	269	/* Time base Upper/Lower (Reading) */
#define TBRL	268
#define TBWU	285	/* Time base Upper/Lower (Writing) */
#define TBWL	284
#define PVR	287	/* Processor Version */

/*
 * mpc8xx-specific special purpose registers of interest here
 */
#define EIE		80
#define EID		81
#define NRI		82
#define IMMR		638
#define IC_CST		560
#define IC_ADR		561
#define IC_DAT		562
#define DC_CST		568
#define DC_ADR		569
#define DC_DAT		570
#define MI_CTR		784
#define MI_AP		786
#define MI_EPN		787
#define MI_TWC		789
#define MI_RPN		790
#define MI_DBCAM	816
#define MI_DBRAM0	817
#define MI_DBRAM1	818
#define MD_CTR		792
#define M_CASID		793
#define MD_AP		794
#define MD_EPN		795
#define M_TWB		796
#define MD_TWC		797
#define MD_RPN		798
#define	M_TW		799
#define	MD_DBCAM	824
#define	MD_DBRAM0	825
#define	MD_DBRAM1	826

/* use of SPRG registers in save/restore */
#define	SAVER0	SPRG0
#define	SAVER1	SPRG1
#define	SAVELR	SPRG2
#define	SAVEXX	SPRG3

/* special instruction definitions */
#define	BDNZ	BC	16,0,
#define	BDNE	BC	0,2,
#define	TLBIA	WORD	$((31<<26)|(370<<1))
#define	MFTB(tbr,d)	WORD	$((31<<26)|((d)<<21)|((tbr&0x1f)<<16)|(((tbr>>5)&0x1f)<<11)|(371<<1))

/* on some models mtmsr doesn't synchronise enough (eg, 603e) */
#define	MSRSYNC	SYNC; ISYNC

#define	UREGSPACE	(UREGSIZE+8)

	TEXT start(SB), $-4

	/* turn off interrupts but enable traps */
	MOVW	MSR, R3
	MOVW	$~(MSR_EE|MSR_FP), R4
	AND	R4, R3
	OR	$(MSR_IP|MSR_ME), R3
	ISYNC
	MOVW	R3, MSR
	MSRSYNC

	MOVW	$0, R0	/* except during trap handling, R0 is zero from now on */
	MOVW	$setSB(SB), R2

/*
 * reset the caches and disable them for now
 */
	MOVW	SPR(IC_CST), R4	/* read and clear */
	MOVW	$(5<<25), R4
	MOVW	R4, SPR(IC_CST)	/* unlock all */
	ISYNC
	MOVW	$(6<<25), R4
	MOVW	R4, SPR(IC_CST)	/* invalidate all */
	ISYNC
	MOVW	$(2<<25), R4
	MOVW	R4, SPR(IC_CST)	/* disable i-cache */
	ISYNC

	SYNC
	MOVW	SPR(DC_CST), R4	/* read and clear */
	MOVW	$(10<<24), R4
	SYNC
	MOVW	R4, SPR(DC_CST)	/* unlock all */
	ISYNC
	MOVW	$(12<<24), R4
	SYNC
	MOVW	R4, SPR(DC_CST)	/* invalidate all */
	ISYNC
	MOVW	$(4<<24), R4
	SYNC
	MOVW	R4, SPR(DC_CST)	/* disable d-cache */
	ISYNC

	MOVW	$7, R4
	MOVW	R4, SPR(158)		/* cancel `show cycle' for normal instruction execution */
	ISYNC

/*
 * set other system configuration values
 */
	MOVW	$INTMEM, R4
	MOVW	R4, SPR(IMMR)		/* set internal memory base */

	MOVW	$mach0(SB), R(MACH)
	ADD	$(MACHSIZE-8), R(MACH), R1	/* set stack */
	SUB	$4, R(MACH), R3
	ADD	$4, R1, R4

clrmach:
	MOVWU	R0, 4(R3)
	CMP	R3, R4
	BNE	clrmach

	MOVW	R0, R(USER)
	MOVW	R0, 0(R(MACH))

	MOVW	$edata(SB), R3
	MOVW	$end(SB), R4
	ADD	$4, R4
	SUB	$4, R3
clrbss:
	MOVWU	R0, 4(R3)
	CMP	R3, R4
	BNE	clrbss

	/* off to main */
	BL	main(SB)
	BR	0(PC)


TEXT	kernelmmu(SB), $0

	/* dont use CASID yet - set to zero for now */
	MOVW	$0, R4
	MOVW	R4, SPR(M_CASID)
	
	/* set Ks = 0 Kp = 1 for all acess groups */
	MOVW	$0x55555555, R4
	MOVW	R4, SPR(MI_AP)
	MOVW	R4, SPR(MD_AP)

	/*
	 * set:
	 *  PowerPC mode
	 *  Page protection mode - no 1K pages
	 *  ~CI when MMU is disbaled
	 *  WT when DMMU is disbaled - this will change
	 *  disable protected TLB for the momment
	 *  ignore user/supervisor state when looking for TLB
	 *  set first tlb entry to 28 - first lockable entry
	 */
	MOVW	$((31<<8)), R4
	MOVW	R4, SPR(MI_CTR)	/* i-mmu control */
	ISYNC
	MOVW	$((31<<8)), R4
	MOVW	R4, SPR(MD_CTR)	/* d-mmu control */
	ISYNC

	/* map various things 1:1 */
	MOVW	$tlbtab(SB), R4
	MOVW	$tlbtabe(SB), R5
	SUB	R4, R5
	MOVW	$(3*4), R6
	DIVW	R6, R5
	SUB	$4, R4
	MOVW	R5, CTR
ltlb:
	MOVWU	4(R4), R5
	MOVW	R5, SPR(MD_EPN)
	MOVW	R5, SPR(MI_EPN)
	MOVWU	4(R4), R5
	MOVW	R5, SPR(MI_TWC)
	MOVW	R5, SPR(MD_TWC)
	MOVWU	4(R4), R5
	MOVW	R5, SPR(MD_RPN)
	MOVW	R5, SPR(MI_RPN)
	BDNZ	ltlb

	/* lock kernel entries in tlb - also reset tlb index*/
	MOVW	$(MMURSV4), R4
	MOVW	R4, SPR(MI_CTR)	/* i-mmu control */
	ISYNC
	MOVW	$(MMURSV4), R4
	MOVW	R4, SPR(MD_CTR)	/* d-mmu control */
	ISYNC

	/* enable i-cache */
	MOVW	$(1<<25), R4
	MOVW	R4, SPR(IC_CST)
	ISYNC

 	/* enable d-cache 	*/
	MOVW	$(1<<25), R4
	MOVW	R4, SPR(DC_CST)
	ISYNC

 	/* enable MMU */
	MOVW	MSR, R4
	OR	$(MSR_IR|MSR_DR), R4
	MOVW	R4, MSR
	ISYNC

	RETURN

TEXT	splhi(SB), $0
	MOVW	MSR, R3
	RLWNM	$0, R3, $~MSR_EE, R4
	SYNC
	MOVW	R4, MSR
	MSRSYNC
	MOVW	LR, R31
	MOVW	R31, 4(R(MACH))	/* save PC in m->splpc */
	RETURN

TEXT	splx(SB), $0
	/* fall though */

TEXT	splxpc(SB), $0
	MOVW	MSR, R4
	RLWMI	$0, R3, $MSR_EE, R4
	RLWNMCC	$0, R3, $MSR_EE, R5
	BNE	splx0
	MOVW	LR, R31
	MOVW	R31, 4(R(MACH))	/* save PC in m->splpc */
splx0:
	SYNC
	MOVW	R4, MSR
	MSRSYNC
	RETURN

TEXT	spllo(SB), $0
	MFTB(TBRL, 3)
	MOVW	R3, spltbl(SB)
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
	MOVW	$(MSR_EE|MSR_PR|MSR_ME|MSR_IP|MSR_IR|MSR_DR|MSR_RI), R4
	MOVW	R4, SPR(SRR1)
	MOVW	R3, R1
	MOVW	R5, SPR(SRR0)
	RFI

/*
 * enter with stack set and mapped.
 * on return, SB (R2) has been set, and R3 has the Ureg*,
 * the MMU has been re-enabled, kernel text and PC are in KSEG,
 * R(MACH) has been set, and R0 contains 0.
 *
 * this can be simplified in the Inferno regime
 */
TEXT	saveureg(SB), $-4
/*
 * save state
 */
	MOVMW	R2, 48(R1)	/* r2:r31 */
	MOVW	$setSB(SB), R2
	MOVW	$mach0(SB), R(MACH)
	MOVW	12(R(MACH)), R(USER)
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
	/* old PC(16) and status(12) saved earlier */
	MOVW	SPR(SAVEXX), R0
	MOVW	R0, 8(R1)	/* cause/vector */
	ADD	$8, R1, R3	/* Ureg* */
	STWCCC	R3, (R1)	/* break any pending reservations */
	MOVW	$0, R0	/* compiler/linker expect R0 to be zero */

	MOVW	MSR, R5
/*	OR	$(MSR_IR|MSR_DR), R5	/* enable MMU */
	MOVW	R5, SPR(SRR1)
	MOVW	LR, R31
/*	OR	$KZERO, R31	/* return PC in KSEG0 */
	MOVW	R31, SPR(SRR0)
	SYNC
	ISYNC
	RFI	/* returns to trap handler */

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
	SYNC
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
	SYNC
	ISYNC
dcf1:
	RETURN

TEXT	tas(SB), $0
	SYNC
	MOVW	R3, R4
	MOVW	$0xdeaddead,R5
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

TEXT	gettbl(SB), $0
	MFTB(TBRL, 3)
	RETURN

TEXT	gettbu(SB), $0
	MOVW	SPR(TBRU), R3
	RETURN

TEXT	getpvr(SB), $0
	MOVW	SPR(PVR), R3
	RETURN

TEXT	getimmr(SB), $0
	MOVW	SPR(IMMR), R3
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

TEXT	getdepn(SB), $0
	MOVW	SPR(MD_EPN), R3
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

TEXT	_flushmmu(SB), $0
	TLBIA
	RETURN

TEXT	_putmmu(SB), $0
	MOVW	MSR, R7
	MOVW	$~(MSR_DR|MSR_IR), R8
	AND	R7, R8
	MOVW	R8, MSR
	OR	$MMUEV, R3
	MOVW	R3, SPR(MD_EPN)
	MOVW	R3, SPR(MI_EPN)
	MOVW	$(MMUWT|MMUV), R5
	MOVW	R5, SPR(MI_TWC)
	MOVW	R5, SPR(MD_TWC)
	MOVW	4(FP), R6
	MOVW	R6, SPR(MD_RPN)
	MOVW	R6, SPR(MI_RPN)
	MOVW	SPR(MD_CTR), R3
	MOVW	R7, MSR
	RETURN

TEXT	gotopc(SB), $0
	MOVW	R3, CTR
	MOVW	LR, R31	/* for trace back */
	BR	(CTR)

TEXT	firmware(SB), $0
	MOVW	MSR, R3
	MOVW	$(MSR_EE|MSR_ME), R4
	ANDN	R4, R3
	OR	$(MSR_IP), R3
	ISYNC
	MOVW	R3, MSR	/* turn off interrupts and machine checks */
	MSRSYNC
	MOVW	$(MSR_RI|MSR_IR|MSR_DR|MSR_ME), R4
	ANDN	R4, R3
	MOVW	R3, SPR(SRR1)
	MOVW	$(0xFF00<<16), R4
	MOVW	R4, SPR(IMMR)
	MOVW	$(0x0800<<16), R4
	MOVW	R4, SPR(SRR0)	/* force bad address */
	MOVW	R0, SPR(149)	/* ensure checkstop on machine check */
	MOVW	R0, R1
	MOVW	R0, R2
	EIEIO
	ISYNC
	RFI

/*
 * byte swapping of arrays of long and short;
 * could possibly be avoided with more changes to drivers
 */
TEXT	swabl(SB), $0
	MOVW	v+4(FP), R4
	MOVW	n+8(FP), R5
	SRAW	$2, R5, R5
	MOVW	R5, CTR
	SUB	$4, R4
	SUB	$4, R3
swabl1:
	ADD	$4, R3
	MOVWU	4(R4), R7
	MOVWBR	R7, (R3)
	BDNZ	swabl1
	RETURN

TEXT	swabs(SB), $0
	MOVW	v+4(FP), R4
	MOVW	n+8(FP), R5
	SRAW	$1, R5, R5
	MOVW	R5, CTR
	SUB	$2, R4
	SUB	$2, R3
swabs1:
	ADD	$2, R3
	MOVHZU	2(R4), R7
	MOVHBR	R7, (R3)
	BDNZ	swabs1
	RETURN

TEXT	legetl(SB), $0
	MOVWBR	(R3), R3
	RETURN

TEXT	lesetl(SB), $0
	MOVW	v+4(FP), R4
	MOVWBR	R4, (R3)
	RETURN

TEXT	legets(SB), $0
	MOVHBR	(R3), R3
	RETURN

TEXT	lesets(SB), $0
	MOVW	v+4(FP), R4
	MOVHBR	R4, (R3)
	RETURN

/*
 * ITLB miss
 *	avoid references that might need the right SB value;
 *	IR and DR are off.
 */
TEXT	itlbmiss(SB), $-4
	MOVW	R1, SPR(M_TW)
	MOVW	SPR(SRR0), R1	/* instruction miss address */
	MOVW	R1, SPR(MD_EPN)
	MOVW	SPR(M_TWB), R1	/* level one pointer */
	MOVW	(R1), R1
	MOVW	R1, SPR(MI_TWC)	/* save level one attributes */
	MOVW	R1, SPR(MD_TWC)	/* save base and attributes */
	MOVW	SPR(MD_TWC), R1	/* level two pointer */
	MOVW	(R1), R1	/* level two entry */
	MOVW	R1, SPR(MI_RPN)	/* write TLB */
	MOVW	SPR(M_TW), R1
	RFI

/*
 * DTLB miss
 *	avoid references that might need the right SB value;
 *	IR and DR are off.
 */
TEXT	dtlbmiss(SB), $-4
	MOVW	R1, SPR(M_TW)
	MOVW	SPR(M_TWB), R1	/* level one pointer */
	MOVW	(R1), R1	/* level one entry */
	MOVW	R1, SPR(MD_TWC)	/* save base and attributes */
	MOVW	SPR(MD_TWC), R1	/* level two pointer */
	MOVW	(R1), R1	/* level two entry */
	MOVW	R1, SPR(MD_RPN)	/* write TLB */
	MOVW	SPR(M_TW), R1
	RFI

/*
 * traps force memory mapping off.
 */
TEXT	trapvec(SB), $-4
traps:
	MOVW	LR, R0

	MOVW	R0, SPR(SAVEXX)	/* vector */
	MOVW	R1, SPR(SAVER1)
	MOVW	CR, R1
	MOVW	MSR, R0
	OR	$(MSR_DR|MSR_IR), R0		/* make data space usable */
	MOVW	R0, MSR
	MOVW	SPR(SRR0), R0	/* save SRR0/SRR1 now, since DLTB might be missing stack page */
	MOVW	R0, LR
	MOVW	SPR(SRR1), R0
	ANDCC	$MSR_PR, R0
	BEQ	ktrap
	
	/* switch to kernel stack */
	MOVW	R1, CR
	MOVW	R2, R0
	MOVW	$setSB(SB), R2
	MOVW	$mach0(SB), R1	/* m-> */
	MOVW	R0, R2
	MOVW	12(R1), R1	/* m->proc */
	MOVW	8(R1), R1	/* m->proc->kstack */
	ADD	$(KSTACK-UREGSIZE), R1
	BR	trap1
ktrap:
	MOVW	R1, CR
	MOVW	SPR(SAVER1), R1
	SUB	$UREGSPACE, R1
trap1:
	MOVW	SPR(SRR1), R0
	MOVW	R0, 12(R1)	/* save status: could take DLTB miss here */
	MOVW	LR, R0
	MOVW	R0, 16(R1)	/* old PC */
	BL	saveureg(SB)
	BL	trap(SB)
	BR	restoreureg

TEXT	intrvec(SB), $-4
	MOVW	LR, R0

/*
 * map data virtually and make space to save
 */
	MOVW	R0, SPR(SAVEXX)	/* vector */
	MOVW	R1, SPR(SAVER1)
	SYNC
	ISYNC
	MOVW	MSR, R0
	OR	$MSR_DR, R0		/* make data space usable */
	SYNC
	MOVW	R0, MSR
	MSRSYNC
	SUB	$UREGSPACE, R1

	MFTB(TBRL, 0)
	MOVW	R0, intrtbl(SB)

	MOVW	SPR(SRR0), R0
	MOVW	R0, LR
	MOVW	SPR(SRR1), R0
	MOVW	R0, 12(R1)
	MOVW	LR, R0
	MOVW	R0, 16(R1)
	BL	saveureg(SB)

	MFTB(TBRL, 5)
	MOVW	R5, isavetbl(SB)

	BL	intr(SB)

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
	MOVW	R0, SPR(SAVELR)	/* LR */
	/* pad, skip */
	MOVW	16(R1), R0
	MOVW	R0, SPR(SRR0)	/* old PC */
	MOVW	12(R1), R0
	MOVW	R0, SPR(SRR1)	/* old MSR */
	/* cause, skip */
	MOVW	44(R1), R1	/* old SP */
	MOVW	SPR(SAVELR), R0
	MOVW	R0, LR
	MOVW	SPR(SAVER0), R0
	RFI

	
GLOBL	mach0+0(SB), $MACHSIZE
GLOBL	spltbl+0(SB), $4
GLOBL	intrtbl+0(SB), $4
GLOBL	isavetbl+0(SB), $4

	RETURN

/*
 * TLB prototype entries, loaded once-for-all at startup,
 * remaining unchanged thereafter.
 * Limit the table to at most 4 entries
 */
#define	TLBE(epn,twc,rpn)	WORD	$(epn);	WORD	$(twc);	WORD	$(rpn)

TEXT	tlbtab(SB), $-4
	/* epn, rpn, twc */
	TLBE(FLASHMEM|MMUEV, MMUPS8M|MMUWT|MMUV, FLASHMEM|MMUPP|MMUSPS|MMUSH|MMUCI|MMUV)	/* FLASH, 8M */
	TLBE(DRAMMEM|MMUEV, MMUPS8M|MMUWT|MMUV, DRAMMEM|MMUPP|MMUSPS|MMUSH|MMUV)	/* DRAM, second 8M */
	TLBE(INTMEM|MMUEV, MMUPS8M|MMUWT|MMUV, INTMEM|MMUPP|MMUSPS|MMUSH|MMUCI|MMUV)	/* IO space 8M */
TEXT	tlbtabe(SB), $-4
	RETURN
