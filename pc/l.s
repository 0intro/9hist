
/*
 *  boot first processor
 */
TEXT	start(SB),$0

	/* clear bss */
	CALL	main(SB)
	/* never returns */


/*
 *  trap vector, each instruction is 4 bytes long
 */
TEXT	traptab(SB),$0

	CALL	noerrcode(SB)	/* divide */
	CALL	noerrcode(SB)	/* debug */
	CALL	noerrcode(SB)	/* non maskable interrupt */
	CALL	noerrcode(SB)	/* breakpoint */
	CALL	noerrcode(SB)	/* overflow */
	CALL	noerrcode(SB)	/* bounds check */
	CALL	noerrcode(SB)	/* invalid opcode */
	CALL	noerrcode(SB)	/* coprocessor not available */
	CALL	errcode(SB)	/* double fault */
	CALL	noerrcode(SB)	/* coprocessor segment overrun */
	CALL	errcode(SB)	/* invalid tss */
	CALL	errcode(SB)	/* segment not present */
	CALL	errcode(SB)	/* stack exception */	
	CALL	errcode(SB)	/* general protection exception */
	CALL	errcode(SB)	/* page fault */
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
