#include "mem.h"

/*
 * Boot first processor
 */
TEXT	start(SB), $-4

	MOVW	$(SUPER|SPL(7)), SR
	MOVL	$a6base(SB), A6
	MOVL	$0, R0
	MOVL	R0, CACR
	MOVL	R0, TACADDR		/* zero tac counter (cause an intr?) */

	MOVL	$mach0(SB), A0
	MOVL	A0, m(SB)
	MOVL	$0, 0(A0)
	MOVL	A0, A7
	ADDL	$(MACHSIZE-4), A7	/* start stack under machine struct */
	MOVL	$0, u(SB)

	MOVL	$vectors(SB), A0
	MOVL	A0, VBR

	BSR	main(SB)
	/* never returns */
dead:
	BRA	dead

/*
 * Take first processor into user mode
 */

TEXT	touser(SB), $-4

	MOVL	$(USERADDR+BY2PG), A7
	MOVW	$0, -(A7)
	MOVL	$(UTZERO+32), -(A7)	/* header is in text */
	MOVW	$0, -(A7)
	MOVL	$(USTKTOP-4*BY2WD), A0
	MOVL	A0, USP
	MOVW	$(SUPER|SPL(0)), SR
	MOVL	$8, R0
	MOVL	R0, CACR
	RTE

TEXT	splhi(SB), $0

	MOVL	m(SB), A0
	MOVL	(A7), 4(A0)
	MOVL	$0, R0
	MOVW	SR, R0
	MOVW	$(SUPER|SPL(7)), SR
	RTS

TEXT	spllo(SB), $0

	MOVL	$0, R0
	MOVW	SR, R0
	MOVW	$(SUPER|SPL(0)), SR
	RTS

TEXT	splx(SB), $0

	MOVL	sr+0(FP), R0
	MOVW	R0, SR
	RTS

TEXT	spldone(SB), $0

	RTS

TEXT	flushcpucache(SB), $0

	MOVL	$(CCLEAR|CENABLE), R0
	MOVL	R0, CACR
	RTS

TEXT	cacrtrap(SB), $0	/* user entry point to control cache, e.g. flush */

	MOVL	R0, CACR
	RTE

TEXT	setlabel(SB), $0

	MOVL	sr+0(FP), A0
	MOVL	A7, (A0)+		/* stack pointer */
	MOVL	(A7), (A0)+		/* pc of caller */
	MOVW	SR, (A0)+		/* status register */
	CLRL	R0			/* ret 0 => not returning */
	RTS

TEXT	gotolabel(SB), $0

	MOVL	p+0(FP), A0
	MOVW	$(SUPER|SPL(7)), SR
	MOVL	(A0)+, A7		/* stack pointer */
	MOVL	(A0)+, (A7)		/* pc; stuff into stack frame */
	MOVW	(A0)+, R0		/* status register */
	MOVW	R0, SR
	MOVL	$1, R0			/* ret 1 => returning */
	RTS

/*
 * Test and set, as a subroutine
 */

TEXT	tas(SB), $0

	MOVL	$0, R0
	MOVL	a+0(FP), A0
	TAS	(A0)
	BEQ	tas_1
	MOVL	$1, R0
tas_1:
	RTS

/*
 * Floating point
 */

TEXT	fpsave(SB), $0

	FSAVE	(fp+0(FP))
	RTS

TEXT	fprestore(SB), $0

	FRESTORE	(fp+0(FP))
	RTS

TEXT	fpregsave(SB), $0

	FMOVEM	$0xFF, (3*4)(fr+0(FP))
	FMOVEMC	$0x7, (fr+0(FP))
	RTS

TEXT	fpregrestore(SB), $0

	FMOVEMC	(fr+0(FP)), $0x7
	FMOVEM	(3*4)(fr+0(FP)), $0xFF
	RTS

TEXT	fpcr(SB), $0

	MOVL	new+0(FP), R1
	MOVL	FPCR, R0
	MOVL	R1, FPCR
	RTS


TEXT	rfnote(SB), $0

	MOVL	uregpp+0(FP), A0
	MOVL	(A0), A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*BY2WD), A7
	MOVL	$-1, R0			/* note causes error in sys call */
	RTE

TEXT	illegal(SB), $0

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	trap(SB)
	ADDL	$4, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*BY2WD), A7
	RTE

TEXT	systrap(SB), $0

	SUBL	$((8+8+1)*BY2WD), A7
	MOVL	A6, ((8+6)*BY2WD)(A7)
	MOVL	R0, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	syscall(SB)
	MOVL	((1+8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVL	((1+8+6)*BY2WD)(A7), A6
	ADDL	$((1+8+8+1)*4), A7
	RTE

TEXT	buserror(SB), $0

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	PEA	((8+8+1+2)*BY2WD)(A7)
	PEA	4(A7)
	BSR	fault68020(SB)
	ADDL	$8, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*4), A7
	RTE

TEXT	tacintr(SB), $0			/* level 1 */

	MOVL	R0, -(A7)
	MOVL	TACADDR, R0
	MOVL	(A7)+, R0
	RTE

TEXT	portintr(SB), $0		/* level 2 */

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	devportintr(SB)
	ADDL	$4, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*BY2WD), A7
	RTE

TEXT	dkintr(SB), $0			/* level 2 */

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	inconintr(SB)
	ADDL	$4, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x77FFF
	ADDL	$((8+8+1)*4), A7
	RTE

TEXT	mouseintr(SB), $0		/* level 4 */

	MOVEM	$0x80C2, -(A7)		/* D0, A0, A1, A6 */
	MOVL	$a6base(SB), A6
	MOVL	$15, R0			/* mask off hex switch */
	ANDB	MOUSE,R0		/* clears quadrature interrupt */
	LEA	mousetab(SB)(R0.W*8), A0
	LEA	mouse(SB), A1
	MOVL	(A0)+, R0
	ADDL	R0, (A1)+		/* dx */
	MOVL	(A0), R0
	ADDL	R0, (A1)+		/* dy */
	ADDL	$1, (A1)		/* track */
	MOVEM	(A7)+, $0x4301
	RTE

TEXT	uartintr(SB), $0		/* level 5 */

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	duartintr(SB)
	ADDL	$4, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*4), A7
	RTE

TEXT	syncintr(SB), $0		/* level 6 */

	SUBL	$((8+8+1)*BY2WD), A7
	MOVEM	$0x7FFF, (A7)
	MOVL	$a6base(SB), A6
	MOVL	USP, A0
	MOVL	A0, ((8+8)*BY2WD)(A7)
	MOVL	A7, -(A7)
	BSR	clock(SB)
	ADDL	$4, A7
	MOVL	((8+8)*BY2WD)(A7), A0
	MOVL	A0, USP
	MOVEM	(A7), $0x7FFF
	ADDL	$((8+8+1)*4), A7
	RTE

GLOBL	mousetab(SB), $128
DATA	mousetab+  0(SB)/4, -1		/* x down,        */
DATA	mousetab+  4(SB)/4,  1		/*         y up   */
DATA	mousetab+  8(SB)/4,  0		/* x -            */
DATA	mousetab+ 12(SB)/4,  1		/*         y up   */
DATA	mousetab+ 16(SB)/4,  1		/* x up           */
DATA	mousetab+ 20(SB)/4,  1		/*         y up   */
DATA	mousetab+ 24(SB)/4,  0		/* x  -           */
DATA	mousetab+ 28(SB)/4,  1		/*         y up   */
DATA	mousetab+ 32(SB)/4, -1		/* x down         */
DATA	mousetab+ 36(SB)/4,  0		/*         y -    */
DATA	mousetab+ 40(SB)/4,  0		/* x -            */
DATA	mousetab+ 44(SB)/4,  0		/*         y -    */
DATA	mousetab+ 48(SB)/4,  1		/* x up,          */
DATA	mousetab+ 52(SB)/4,  0		/*         y -    */
DATA	mousetab+ 56(SB)/4,  0		/* x -            */
DATA	mousetab+ 60(SB)/4,  0		/*         y -    */
DATA	mousetab+ 64(SB)/4, -1		/* x down         */
DATA	mousetab+ 68(SB)/4, -1		/*         y down */
DATA	mousetab+ 72(SB)/4,  0		/* x -            */
DATA	mousetab+ 76(SB)/4, -1		/*         y down */
DATA	mousetab+ 80(SB)/4,  1		/* x up           */
DATA	mousetab+ 84(SB)/4, -1		/*         y down */
DATA	mousetab+ 88(SB)/4,  0		/* x -            */
DATA	mousetab+ 92(SB)/4, -1		/*         y down */
DATA	mousetab+ 96(SB)/4, -1		/* x down         */
DATA	mousetab+100(SB)/4,  0		/*         y -    */
DATA	mousetab+104(SB)/4,  0		/* x -            */
DATA	mousetab+108(SB)/4,  0		/*         y -    */
DATA	mousetab+112(SB)/4,  1		/* x up           */
DATA	mousetab+116(SB)/4,  0		/*         y -    */
DATA	mousetab+120(SB)/4,  0		/* x -            */
DATA	mousetab+124(SB)/4,  0		/*         y -    */

GLOBL	mach0+0(SB), $MACHSIZE
GLOBL	u(SB), $4
GLOBL	m(SB), $4
