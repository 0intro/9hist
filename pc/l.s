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
	 WORD	$lowcore-KZERO(SB)
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
	 WORD	$tgdtptr-KZERO(SB)
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

/*	JMPFAR	SELECTOR(2, SELGDT, 0):$mode32bit(SB) /**/
	 BYTE	$0xEA
	 WORD	$mode32bit-KZERO(SB)
	 WORD	$SELECTOR(2, SELGDT, 0)

TEXT	mode32bit(SB),$0

	/*
	 * Clear BSS
	 */
	LEAL	edata-KZERO(SB),SI
	MOVL	SI,DI
	ADDL	$4,DI
	MOVL	$0,AX
	MOVL	AX,(SI)
	LEAL	end-KZERO(SB),CX
	SUBL	DI,CX
	SHRL	$2,CX
	REP
	MOVSL

	/*
	 *  make a bottom level page table page that maps the first
	 *  4 meg of physical memory
	 */
	LEAL	tpt-KZERO(SB),AX	/* get phys addr of temporary page table */
	ADDL	$(BY2PG-1),AX		/* must be page alligned */
	ANDL	$(~(BY2PG-1)),AX	/* ... */
	MOVL	$1024,CX		/* pte's per page */
	MOVL	$(((1024-1)<<12)|PTEVALID|PTEKERNEL|PTEWRITE),BX
setpte:
	MOVL	BX,-4(AX)(CX*4)
	SUBL	$(1<<12),BX
	LOOP	setpte

	/*
	 *  make a top level page table page that maps the first
	 *  4 meg of memory to 0 thru 4meg and to KZERO thru KZERO+4meg
	 */
	MOVL	AX,BX
	ADDL	$BY2PG,AX
	MOVL	BX,0(AX)
	MOVL	BX,((KZERO>>22)&(BY2PG-1))(AX)

	/*
	 *  point processor to top level page & turn on paging
	 */
	MOVL	AX,CR3
	MOVL	CR0,AX
	ORL	$1,AX
	MOVL	AX,CR0

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
GLOBL	tpt(SB), $(BY2PG*3)

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
	LONG	$tgdt-KZERO(SB)

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

/*
 *  test and set
 */
TEXT	tas(SB),$0
	MOVL	$0xdeadead,AX
	MOVL	l+0(FP),BX
	XCHGL	AX,(BX)
	RET

/*
 *  load the idt
 */
GLOBL	idtptr(SB),$6
TEXT	lidt(SB),$0
	MOVL	t+0(FP),AX
	MOVL	AX,idtptr+2(SB)
	MOVL	l+4(FP),AX
	MOVW	AX,idtptr(SB)
	MOVL	idtptr(SB),IDTR
	RET

/*
 *  load the gdt
 */
GLOBL	gdtptr(SB),$6
TEXT	lgdt(SB),$0
	MOVL	t+0(FP),AX
	MOVL	AX,gdtptr+2(SB)
	MOVL	l+4(FP),AX
	MOVW	AX,gdtptr(SB)
	MOVL	gdtptr(SB),GDTR
	RET

/*
 *  special traps
 */
TEXT	intr0(SB),$0
	PUSHL	$0
	PUSHL	$0
	JMP	intrcommon
TEXT	intr1(SB),$0
	PUSHL	$0
	PUSHL	$1
	JMP	intrcommon
TEXT	intr2(SB),$0
	PUSHL	$0
	PUSHL	$2
	JMP	intrcommon
TEXT	intr3(SB),$0
	PUSHL	$0
	PUSHL	$3
	JMP	intrcommon
TEXT	intr4(SB),$0
	PUSHL	$0
	PUSHL	$4
	JMP	intrcommon
TEXT	intr5(SB),$0
	PUSHL	$0
	PUSHL	$5
	JMP	intrcommon
TEXT	intr6(SB),$0
	PUSHL	$0
	PUSHL	$6
	JMP	intrcommon
TEXT	intr7(SB),$0
	PUSHL	$0
	PUSHL	$7
	JMP	intrcommon
TEXT	intr8(SB),$0
	PUSHL	$8
	JMP	intrscommon
TEXT	intr9(SB),$0
	PUSHL	$0
	PUSHL	$9
	JMP	intrcommon
TEXT	intr10(SB),$0
	PUSHL	$10
	JMP	intrscommon
TEXT	intr11(SB),$0
	PUSHL	$11
	JMP	intrscommon
TEXT	intr12(SB),$0
	PUSHL	$12
	JMP	intrscommon
TEXT	intr13(SB),$0
	PUSHL	$13
	JMP	intrscommon
TEXT	intr14(SB),$0
	PUSHL	$14
	JMP	intrscommon
TEXT	intr15(SB),$0
	PUSHL	$0
	PUSHL	$15
	JMP	intrcommon
TEXT	intr16(SB),$0
	PUSHL	$0
	PUSHL	$16
	JMP	intrcommon
TEXT	intr17(SB),$0
	PUSHL	$0
	PUSHL	$17
	JMP	intrcommon
TEXT	intr18(SB),$0
	PUSHL	$0
	PUSHL	$18
	JMP	intrcommon
TEXT	intr19(SB),$0
	PUSHL	$0
	PUSHL	$19
	JMP	intrcommon
TEXT	intr20(SB),$0
	PUSHL	$0
	PUSHL	$20
	JMP	intrcommon
TEXT	intr21(SB),$0
	PUSHL	$0
	PUSHL	$21
	JMP	intrcommon
TEXT	intr22(SB),$0
	PUSHL	$0
	PUSHL	$22
	JMP	intrcommon
TEXT	intr23(SB),$0
	PUSHL	$0
	PUSHL	$23
	JMP	intrcommon
TEXT	intr64(SB),$0
	PUSHL	$0
	PUSHL	$64
	JMP	intrcommon
TEXT	intrbad(SB),$0
	PUSHL	$0
	PUSHL	$0x1ff
	JMP	intrcommon

intrcommon:
	PUSHL	DS
	PUSHAL
	LEAL	0(SP),AX
	PUSHL	AX
	CALL	trap(SB)
	POPL	AX
	POPAL
	POPL	DS
	ADDL	$8,SP	/* error code and trap type */
	IRETL
	RET

intrscommon:
	PUSHL	DS
	PUSHAL
	LEAL	0(SP),AX
	PUSHL	AX
	CALL	trap(SB)
	POPL	AX
	POPAL
	POPL	DS
	ADDL	$8,SP	/* error code and trap type */
	IRETL
	RET

/*
 *  turn interrupts and traps on
 */
TEXT	spllo(SB),$0
	PUSHFL
	POPL	AX
	STI
	RET

/*
 *  turn interrupts and traps off
 */
TEXT	splhi(SB),$0
	PUSHFL
	POPL	AX
	CLI
	RET

/*
 *  set interrupt level
 */
TEXT	splx(SB),$0
	MOVL	s+0(FP),AX
	PUSHL	AX
	POPFL
	RET

/*
 *  do nothing whatsoever till interrupt happens
 */
TEXT	idle(SB),$0
	HLT
	RET

/*
 *  label consists of a stack pointer and a PC
 */
TEXT	gotolabel(SB),$0
	MOVL	l+0(FP),AX
	MOVL	0(AX),SP	/* restore sp */
	MOVL	4(AX),AX	/* put return pc on the stack */
	MOVL	AX,0(SP)
	RET

TEXT	setlabel(SB),$0
	MOVL	l+0(FP),AX
	MOVL	SP,0(AX)	/* store sp */
	MOVL	0(SP),BX	/* store return pc */
	MOVL	BX,4(AX)
	RET
