
/*
 *  boot first processor
 */
TEXT	start(SB),$0

	/* clear bss */
	CALL	main(SB)
	/* never returns */


/*
 *  standard traps
 */
divtrap:
	PUSHL	$0
	PUSHL	$0
	JMP	alltrap
debtrap:
	PUSHL	$0
	PUSHL	$1
	JMP	alltrap
nmitrap:
	PUSHL	$0
	PUSHL	$2
	JMP	alltrap
bptrap:
	PUSHL	$0
	PUSHL	$3
	JMP	alltrap
oftrap:
	PUSHL	$0
	PUSHL	$4
	JMP	alltrap
boundtrap:
	PUSHL	$0
	PUSHL	$5
	JMP	alltrap
invtrap:
	PUSHL	$0
	PUSHL	$6
	JMP	alltrap
nocotrap:
	PUSHL	$0
	PUSHL	$7
	JMP	alltrap
dfault:
	PUSHL	$8
	JMP	alltrap
csotrap:
	PUSHL	$0
	PUSHL	$9
	JMP	alltrap
tsstrap:
	PUSHL	$0
	PUSHL	$10
	JMP	alltrap
segtrap:
	PUSHL	$11
	JMP	alltrap
stacktrap:
	PUSHL	$12
	JMP	alltrap
prottrap:
	PUSHL	$13
	JMP	alltrap
pagefault:
	PUSHL	$14
	JMP	alltrap
cetrap:
	PUSHL	$0
	PUSHL	$15
	JMP	alltrap
	CALL	noerrcode(SB)	/* coprocessor error */
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)
	CALL	noerrcode(SB)

TEXT	noerrcode(SB),$0

	PUSHL	EAX
	MOVL	4(ESP),EAX
/*	JMP	george /**/

TEXT	errcode(SB),$0

	XCHGL	(ESP),EAX
	/* fall through */

TEXT	saveregs(SB),$0

george:
	PUSHL	EBX
	MOVL	$noerrcode(SB),EBX	/* calculate trap number */
	SUBL	EAX,EBX			/* ... */
	SHRL	$1,EBX			/* ... */
	PUSHL	ECX
	PUSHL	EDX
	PUSHL	EBP
	PUSHL	ESI
	PUSHL	EDI
	PUSHL	EBX			/* save trap number */
	CALL	trap(SB)
	ADDL	$4,ESP			/* drop trap number */
	POPL	EDI
	POPL	ESI
	POPL	EBP
	POPL	EDX
	POPL	ECX
	POPL	EBX
	POPL	EAX
	ADDL	$4,ESP			/* drop error code */
	IRET

TEXT	trap(SB),$0

	RET

TEXT	main(SB),$0

	RET
