#include "mem.h"

/*
 *	about to walk all over ms/dos - turn off interrupts
 */
TEXT	origin(SB),$0

	CLI

/*
 *	move the first 1k bytes down to low core and jump to them
 *	- looks wierd because it is being assembled by a 32 bit
 *	  assembler for a 16 bit world
 */
	MOVL	$0,BX
	INCL	BX
	SHLL	$(10-1),BX
	MOVL	BX,CX
	MOVL	$0,SI
	MOVW	SI,ES
	MOVL	SI,DI
	CLD
	REP
	MOVSL
/*	JMPFAR	00:$lowcore(SB) /**/
	 BYTE	$0xEA
	 WORD	$lowcore(SB)
	 WORD	$0

TEXT	lowcore(SB),$0

/*
 *	move the next 63K down
 */
	MOVL	$0,CX
	INCL	CX
	SHLL	$(15-1),CX
	SUBL	BX,CX
	SHLL	$1,BX
	MOVL	BX,SI
	MOVL	BX,DI
	REP
	MOVSL

/*
 *	now that we're in low core, update the DS
 */

	MOVL	$0,BX
	MOVW	BX,DS

/*
 * 	goto protected mode
 */
/*	MOVL	tgdtptr(SB),GDTR /**/
	 BYTE	$0x0f
	 BYTE	$0x01
	 BYTE	$0x16
	 WORD	$tgdtptr(SB)
	MOVL	CR0,AX
	ORL	$1,AX
	MOVL	AX,CR0

/*
 *	clear prefetch queue (wierd code to avoid optimizations)
 */
	CLC
	JCC	flush
	MOVL	AX,AX
flush:

/*
 *	set all segs
 */
/*	MOVW	$SELECTOR(1, SELGDT, 0),AX	/**/
	 BYTE	$0xc7
	 BYTE	$0xc0
	 WORD	$SELECTOR(1, SELGDT, 0)
	MOVW	AX,DS
	MOVW	AX,SS
	MOVW	AX,ES

/*	JMPFAR*	SELECTOR(2, SELGDT, 0):$protected(SB) /**/
	 BYTE	$0xEA
	 WORD	$mode32bit(SB)
	 WORD	$SELECTOR(2, SELGDT, 0)

TEXT	mode32bit(SB),$0

	/*
	 * Clear BSS
	 */
	LEAL	edata(SB),SI
	LEAL	edata+4(SB),DI
	MOVL	$0,AX
	MOVL	AX,(SI)
	LEAL	end(SB),CX
	SUBL	DI,CX
	SHRL	$2,CX
	REP
	MOVSL

	/*
	 *  stack and mach
	 */
	MOVL	$mach0(SB),SP
	MOVL	SP,m(SB)
	MOVL	$0,0(SP)
	ADDL	$(MACHSIZE-4),SP	/* start stack under machine struct */
	MOVL	$0, u(SB)

	CALL	main(SB)

loop:
	JMP	loop

GLOBL	mach0+0(SB), $MACHSIZE
GLOBL	u(SB), $4
GLOBL	m(SB), $4

/*
 *  gdt to get us to 32-bit/segmented/unpaged mode
 */
TEXT	tgdt(SB),$0

	/* null descriptor */
	LONG	$0
	LONG	$0

	/* data segment descriptor for 4 gigabytes (PL 0) */
	LONG	$(0xFFFF)
	LONG	$(SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(0)|SEGDATA|SEGW)

	/* exec segment descriptor for 4 gigabytes (PL 0) */
	LONG	$(0xFFFF)
	LONG	$(SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(0)|SEGEXEC|SEGR)

/*
 *  pointer to initial gdt
 */
TEXT	tgdtptr(SB),$0

	WORD	$(3*8)
	LONG	$tgdt(SB)

/*
 *  input a byte
 */
TEXT	inb(SB),$0

	MOVL	p+0(FP),DX
	XORL	AX,AX
	INB
	RET

/*
 *  output a byte
 */
TEXT	outb(SB),$0

	MOVL	p+0(FP),DX
	MOVL	b+4(FP),AX
	OUTB
	RET
