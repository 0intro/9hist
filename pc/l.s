
/*
 *  boot processor
 */
TEXT	start(SB),$0

	/* clear bss */
	CALL	main(SB)
	/* never returns */

/*
 *  first 16 ``standard'' traps
 */
TEXT	trap0(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$0
	JMP	alltrap

TEXT	trap1(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$1
	JMP	alltrap

TEXT	trap2(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$2
	JMP	alltrap

TEXT	trap3(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$3
	JMP	alltrap

TEXT	trap4(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$4
	JMP	alltrap

TEXT	trap5(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$5
	JMP	alltrap

TEXT	trap6(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$6
	JMP	alltrap

TEXT	trap7(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$7
	JMP	alltrap

TEXT	trap8(SB),$0

	PUSHL	$8
	JMP	alltrap

TEXT	trap9(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$9
	JMP	alltrap

TEXT	trap10(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$10
	JMP	alltrap

TEXT	trap11(SB),$0

	PUSHL	$11
	JMP	alltrap

TEXT	trap12(SB),$0

	PUSHL	$12
	JMP	alltrap

TEXT	trap13(SB),$0

	PUSHL	$13
	JMP	alltrap

TEXT	trap14(SB),$0

	PUSHL	$14
	JMP	alltrap

TEXT	trap15(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$15
	JMP	alltrap

/*
 *  invalid trap
 */
TEXT	invtrap(SB),$0

	PUSHL	$0	/* put on an error code */
	PUSHL	$16
	JMP	alltrap

/*
 *  common trap code
 */
alltrap:

	PUSHAL
	CALL	trap(SB)
	POPAL
	ADDL	$#8,SP		/* pop the trap and error codes */
	IRETL

/*
 *  stubs
 */
TEXT	main(SB),$0

	RET

TEXT	trap(SB),$0

	RET
