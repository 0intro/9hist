#include "mem.h"

#define	SYSPSR	(SPL(0x0)|PSREF|PSRSUPER)

TEXT	start(SB), $-4

	/* get virtual, fast */
	/* we are executing in segment 0, mapped to pmeg 0. stack is there too */
	/* get virtual by mapping segment(KZERO) to pmeg 0., and next to 1 */
	MOVW	$KZERO, R7
	MOVB	R0, (R7, 3)
	MOVW	$(KZERO+BY2SEGM), R7
	MOVW	$1, R8
	MOVB	R8, (R7, 3)
	/* now mapped correctly.  jmpl to where we want to be */
	MOVW	$setSB(SB), R2
	MOVW	$startvirt(SB), R7
	JMPL	(R7)
	RETURN			/* can't get here */

TEXT	startvirt(SB), $-4

	MOVW	$BOOTSTACK, R1
	MOVW	$mach0(SB), R(MACH)
	MOVW	$0x8, R7
	MOVW	R7, WIM
	JMPL	main(SB)
	MOVW	(R0), R0
	RETURN

TEXT	swap1(SB), $0

	MOVW	keyaddr+0(FP), R8
	TAS	(R8), R7		/* LDSTUB, thank you ken */
	RETURN

TEXT	swap1_should_work(SB), $0

	MOVW	keyaddr+0(FP), R8
	MOVW	$1, R7
	SWAP	(R8), R7
	RETURN

TEXT	swap1x(SB), $0

	MOVW	keyaddr+0(FP), R8
	MOVW	PSR, R9
	MOVW	R9, R10
	AND	$~PSRET, R10		/* BUG: book says this is buggy */
	MOVW	R10, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	MOVW	(R8), R7
	CMP	R7, R0
	BNE	was1
	MOVW	$1, R10
	MOVW	R10, (R8)
was1:
	MOVW	R9, PSR
	RETURN

TEXT	spllo(SB), $0

	MOVW	PSR, R7
	MOVW	R7, R10
	OR	$PSRET, R10
	MOVW	R10, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	RETURN

TEXT	splhi(SB), $0

	MOVW	PSR, R7
	MOVW	R7, R10
	AND	$~PSRET, R10	/* BUG: book says this is buggy */
	MOVW	R10, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	RETURN

TEXT	splx(SB), $0

	MOVW	psr+0(FP), R7
	MOVW	R7, PSR		/* BUG: book says this is buggy */
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	RETURN

TEXT	touser(SB), $-4

	MOVW	$SYSPSR, R7
	MOVW	R7, PSR

	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	MOVW	sp+0(FP), R1
	SAVE	R0, R0			/* RETT is implicit RESTORE */
	MOVW	$(UTZERO+32), R7	/* PC; header appears in text */
	MOVW	$(UTZERO+32+4), R8	/* nPC */
	RETT	R7, R8

TEXT	rfnote(SB), $0

	MOVW	0(FP), R1		/* 1st arg is &uregpointer */
	ADD	$4, R1			/* point at ureg */
	JMP	restore

TEXT	traplink(SB), $-4

	/* R8 to R23 are free to play with */
	/* R17 contains PC, R18 contains nPC */
	/* R19 has PSR loaded from vector code */
	ANDCC	$PSRPSUPER, R19, R0
	BE	usertrap

kerneltrap:
	/*
	 * Interrupt or fault from kernel
	 */
	ANDN	$7, R1, R20			/* dbl aligned */
	MOVW	R1, (0-(4*(32+6))+(4*1))(R20)	/* save R1=SP */
	/* really clumsy: store these in Ureg so can be restored below */
	MOVW	R2, (0-(4*(32+6))+(4*2))(R20)	/* SB */
	MOVW	R5, (0-(4*(32+6))+(4*5))(R20)	/* USER */
	MOVW	R6, (0-(4*(32+6))+(4*6))(R20)	/* MACH */
	SUB	$(4*(32+6)), R20, R1

trap1:
	MOVW	Y, R20
	MOVW	R20, (4*(32+0))(R1)		/* Y */
	MOVW	TBR, R20
	MOVW	R20, (4*(32+1))(R1)		/* TBR */
	AND	$~0x1F, R19			/* force CWP=0 */
	MOVW	R19, (4*(32+2))(R1)		/* PSR */
	MOVW	R18, (4*(32+3))(R1)		/* nPC */
	MOVW	R17, (4*(32+4))(R1)		/* PC */
	MOVW	R0, (4*0)(R1)
	MOVW	R3, (4*3)(R1)
	MOVW	R4, (4*4)(R1)
	MOVW	R7, (4*7)(R1)
	RESTORE	R0, R0
	/* now our registers R8-R31 are same as before trap */
	/* save registers two at a time */
	MOVD	R8, (4*8)(R1)
	MOVD	R10, (4*10)(R1)
	MOVD	R12, (4*12)(R1)
	MOVD	R14, (4*14)(R1)
	MOVD	R16, (4*16)(R1)
	MOVD	R18, (4*18)(R1)
	MOVD	R20, (4*20)(R1)
	MOVD	R22, (4*22)(R1)
	MOVD	R24, (4*24)(R1)
	MOVD	R26, (4*26)(R1)
	MOVD	R28, (4*28)(R1)
	MOVD	R30, (4*30)(R1)
	/* SP and SB and u and m are already set; away we go */
	MOVW	R1, -4(R1)		/* pointer to Ureg */
	SUB	$8, R1
	MOVW	$SYSPSR, R7
	MOVW	R7, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	JMPL	faultsparc(SB)

	ADD	$8, R1
restore:
	MOVW	(4*(32+2))(R1), R8		/* PSR */
	MOVW	R8, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0

	MOVD	(4*30)(R1), R30
	MOVD	(4*28)(R1), R28
	MOVD	(4*26)(R1), R26
	MOVD	(4*24)(R1), R24
	MOVD	(4*22)(R1), R22
	MOVD	(4*20)(R1), R20
	MOVD	(4*18)(R1), R18
	MOVD	(4*16)(R1), R16
	MOVD	(4*14)(R1), R14
	MOVD	(4*12)(R1), R12
	MOVD	(4*10)(R1), R10
	MOVD	(4*8)(R1), R8
	SAVE	R0, R0
	MOVD	(4*6)(R1), R6
	MOVD	(4*4)(R1), R4
	MOVD	(4*2)(R1), R2
	MOVW	(4*(32+0))(R1), R20		/* Y */
	MOVW	R20, Y
	MOVW	(4*(32+4))(R1), R17		/* PC */
	MOVW	(4*(32+3))(R1), R18		/* nPC */
	MOVW	(4*1)(R1), R1	/* restore R1=SP */
	RETT	R17, R18
	
usertrap:
	/*
	 * Interrupt or fault from user
	 */
	MOVW	R1, R8
	MOVW	R2, R9
	MOVW	$setSB(SB), R2
	MOVW	$(USERADDR+BY2PG), R1
	MOVW	R8, (0-(4*(32+6))+(4*1))(R1)	/* save R1=SP */
	MOVW	R9, (0-(4*(32+6))+(4*2))(R1)	/* save R2=SB */
	MOVW	R5, (0-(4*(32+6))+(4*5))(R1)	/* save R5=USER */
	MOVW	R6, (0-(4*(32+6))+(4*6))(R1)	/* save R6=MACH */
	MOVW	$USERADDR, R(USER)
	MOVW	$mach0(SB), R(MACH)
	SUB	$(4*(32+6)), R1
	JMP	trap1

TEXT	syslink(SB), $-4

	/* R8 to R23 are free to play with */
	/* R17 contains PC, R18 contains nPC */
	/* R19 has PSR loaded from vector code */
	/* assume user did it; syscall checks */
	MOVW	R1, R8
	MOVW	R2, R9
	MOVW	$setSB(SB), R2
	MOVW	$(USERADDR+BY2PG), R1
	MOVW	R8, (0-(4*(32+6))+4)(R1)	/* save R1=SP */
	SUB	$(4*(32+6)), R1
	MOVW	R9, (4*2)(R1)			/* save R2=SB */
	MOVW	R3, (4*3)(R1)			/* global register */
	MOVD	R4, (4*4)(R1)			/* global register, R5=USER */
	MOVD	R6, (4*6)(R1)			/* save R6=MACH, R7=syscall# */
	MOVW	$USERADDR, R(USER)
	MOVW	$mach0(SB), R(MACH)
	MOVW	TBR, R20
	MOVW	R20, (4*(32+1))(R1)		/* TBR */
	AND	$~0x1F, R19
	MOVW	R19, (4*(32+2))(R1)		/* PSR */
	MOVW	R18, (4*(32+3))(R1)		/* nPC */
	MOVW	R17, (4*(32+4))(R1)		/* PC */
	RESTORE	R0, R0
	/* now our registers R8-R31 are same as before trap */
	MOVW	R15, (4*15)(R1)
	/* SP and SB and u and m are already set; away we go */
	MOVW	R1, -4(R1)		/* pointer to Ureg */
	SUB	$8, R1
	MOVW	$SYSPSR, R7
	MOVW	R7, PSR
	JMPL	syscall(SB)
	/* R7 contains return value from syscall */

	ADD	$8, R1
	MOVW	(4*(32+2))(R1), R8		/* PSR */
	MOVW	R8, PSR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0

	MOVW	(4*15)(R1), R15
	SAVE	R0, R0
	MOVW	(4*6)(R1), R6
	MOVD	(4*4)(R1), R4
	MOVD	(4*2)(R1), R2
	MOVW	(4*(32+4))(R1), R17		/* PC */
	MOVW	(4*(32+3))(R1), R18		/* nPC */
	MOVW	(4*1)(R1), R1	/* restore R1=SP */
	RETT	R17, R18

TEXT	puttbr(SB), $0

	MOVW	tbr+0(FP), R7
	MOVW	R7, TBR
	OR	R0, R0
	OR	R0, R0
	OR	R0, R0
	RETURN

TEXT	gettbr(SB), $0

	MOVW	TBR, R7
	RETURN

TEXT	r1(SB), $0

	MOVW	R1, R7
	RETURN

TEXT	getwim(SB), $0

	MOVW	WIM, R7
	RETURN

TEXT	setlabel(SB), $0

	MOVW	b+0(FP), R7
	MOVW	R1, (R7)
	MOVW	R15, 4(R7)
	MOVW	$0, R7
	RETURN

TEXT	gotolabel(SB), $0

	MOVW	b+0(FP), R8
	MOVW	(R8), R1
	MOVW	4(R8), R15
	MOVW	R15, 0(R1)
	MOVW	$1, R7
	RETURN

TEXT	putcxsegm(SB), $0

	MOVW	0(FP), R8		/* context */
	MOVW	4(FP), R9		/* segment addr */
	MOVW	8(FP), R10		/* segment value */
	MOVW	$0xFFE80118, R7
	JMPL	(R7)
	RETURN

TEXT	putcxreg(SB), $0

	MOVW	$CONTEXT, R7
	MOVW	0(FP), R8
	MOVB	R8, (R7, 2)
	RETURN

TEXT	putb2(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVB	R8, (R7, 2)
	RETURN

TEXT	getb2(SB), $0

	MOVW	0(FP), R7
	MOVB	(R7, 2), R7
	RETURN

TEXT	getw2(SB), $0

	MOVW	0(FP), R7
	MOVW	(R7, 2), R7
	RETURN

TEXT	putw2(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 2)
	RETURN

TEXT	putw4(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 4)
	RETURN

TEXT	putwC(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 0xC)
	RETURN

TEXT	putwD(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 0xD)
	RETURN

TEXT	putwD16(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xD)
	RETURN

TEXT	putwE(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 0xE)
	RETURN

TEXT	putwE16(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	ADD	$(1<<4), R7
	MOVW	R8, (R7, 0xE)
	RETURN

TEXT	putsegm(SB), $0

	MOVW	0(FP), R7
	MOVW	4(FP), R8
	MOVW	R8, (R7, 3)
	RETURN

TEXT	clearfpintr(SB), $0
	MOVW	$fpq+BY2WD(SB), R7
	ANDN	$0x7, R7		/* must be D aligned */
	MOVW	$fpr+0(SB), R9
clrq:
	MOVD	FQ, (R7)
	MOVW	FSR, (R9)
	MOVW	(R9), R8
	AND	$(1<<13), R8		/* queue not empty? */
	BNE	clrq
	RETURN

TEXT	getfsr(SB), $0
	MOVW	$fpr+0(SB), R7
	MOVW	FSR, (R7)
	MOVW	(R7), R7
	RETURN

GLOBL	mach0+0(SB), $MACHSIZE
GLOBL	fpq+0(SB), $(3*BY2WD)
GLOBL	fpr+0(SB), $BY2WD
