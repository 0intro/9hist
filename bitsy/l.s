#include "mem.h"

/*
 * Entered here from Compaq's bootldr with MMU disabled.
 */
TEXT _start(SB), $-4
	MOVW	$setR12(SB), R12		/* load the SB */
_main:
	/* SVC mode, interrupts disabled */
	MOVW	$(PsrDirq|PsrDfiq|PsrMsvc), R1
	MOVW	R1, CPSR

	/* flush TLB's */
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0)
	/* drain prefetch */
	MOVW	R0,R0						
	MOVW	R0,R0
	MOVW	R0,R0
	MOVW	R0,R0

	/* drain write buffer */
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0), 4

	/* disable the MMU */
	MOVW	$0x130, R1
	MCR     CpMMU, 0, R1, C(CpControl), C(0x0)

	MOVW	$(MACHADDR+BY2PG), R13		/* stack */
	SUB	$4, R13				/* link */
	BL	main(SB)
	BL	exit(SB)
	/* we shouldn't get here */
_mainloop:
	B	_mainloop
	BL	_div(SB)			/* hack to get _div etc loaded */

/* flush tlb's */
TEXT flushmmu(SB), $-4
	MCR	CpMMU, 0, R0, C(CpTLBFlush), C(0x0)
	RET

/* flush instruction cache */
TEXT flushicache(SB), $-4
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0)
	/* drain prefetch */
	MOVW	R0,R0					
	MOVW	R0,R0
	MOVW	R0,R0
	MOVW	R0,R0
	RET

/* flush data cache */
TEXT flushdcache(SB), $-4
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0)
	RET

/* flush i and d caches */
TEXT flushcache(SB), $-4
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0)
	/* drain prefetch */
	MOVW	R0,R0						
	MOVW	R0,R0
	MOVW	R0,R0
	MOVW	R0,R0
	RET

/* drain write buffer */
TEXT wbflush(SB), $-4
	MCR	CpMMU, 0, R0, C(CpCacheFlush), C(0x0), 4
	RET

/* return cpu id */
TEXT getcpuid(SB), $-4
	MRC	CpMMU, 0, R0, C(CpCPUID), C(0x0)
	RET

/* return fault status */
TEXT getfsr(SB), $-4
	MRC	CpMMU, 0, R0, C(CpFSR), C(0x0)
	RET

/* return fault address */
TEXT getfar(SB), $-4
	MRC	CpMMU, 0, R0, C(CpFAR), C(0x0)
	RET

/* set the translation table base */
TEXT putttb(SB), $-4
	MCR	CpMMU, 0, R0, C(CpTTB), C(0x0)
	RET

/*
 *  enable mmu, i and d caches, and exception vectors at 0xffff0000
 */
TEXT mmuenable(SB), $-4
	MRC	CpMMU, 0, R0, C(CpControl), C(0x0)
	ORR	$(CpCmmuena|CpCdcache|CpCicache|CpCvivec), R0
	MCR     CpMMU, 0, R0, C(CpControl), C(0x0)
	RET

TEXT mmudisable(SB), $-4
	MRC	CpMMU, 0, R0, C(CpControl), C(0x0)
	BIC	$(CpCmmuena|CpCdcache|CpCicache|CpCvivec), R0
	MCR     CpMMU, 0, R0, C(CpControl), C(0x0)
	RET

/* set the translation table base */
TEXT putdac(SB), $-4
	MCR	CpMMU, 0, R0, C(CpDAC), C(0x0)
	RET

/* set address translation pid */
TEXT putpid(SB), $-4
	MCR	CpMMU, 0, R0, C(CpPID), C(0x0)
	RET

/*
 *  set the stack value for the mode passed in R0
 */
TEXT setr13(SB), $-4
	MOVW	4(FP), R1

	MOVW	CPSR, R2
	BIC	$PsrMask, R2, R3
	ORR	R0, R3
	MOVW	R3, CPSR

	MOVW	R13, R0
	MOVW	R1, R13

	MOVW	R2, CPSR
	RET

/*
 *  exception vectors, copied by trapinit() to somewhere useful
 */
TEXT exceptionvectors(SB), $-4
	MOVW	0x18(R15), R15		/* reset */
	MOVW	0x18(R15), R15		/* undefined */
	MOVW	0x18(R15), R15		/* SWI */
	MOVW	0x18(R15), R15		/* prefetch abort */
	MOVW	0x18(R15), R15		/* data abort */
	MOVW	0x18(R15), R15		/* reserved */
	MOVW	0x18(R15), R15		/* IRQ */
	MOVW	0x18(R15), R15		/* FIQ */
	WORD	$_vsvc(SB)		/* reset, in svc mode already */
	WORD	$_vund(SB)		/* undefined, switch to svc mode */
	WORD	$_vsvc(SB)		/* swi, in svc mode already */
	WORD	$_vpab(SB)		/* prefetch abort, switch to svc mode */
	WORD	$_vdab(SB)		/* data abort, switch to svc mode */
	WORD	$_vsvc(SB)		/* reserved */
	WORD	$_virq(SB)		/* IRQ, switch to svc mode */
	WORD	$_vfiq(SB)		/* FIQ, switch to svc mode */

TEXT _vsvc(SB), $-4			/* reset or SWI or reserved */
	SUB	$12, R13		/* make room for pc, psr, & type */
	MOVW	R14, 8(R13)		/* ureg->pc = interupted PC */
	MOVW	SPSR, R14		/* ureg->psr = SPSR */
	MOVW	R14, 4(R13)		/* ... */
	MOVW	$PsrMsvc, R14		/* ureg->type = PsrMsvc */
	MOVW	R14, (R13)		/* ... */
	MOVM.DB.W.S [R0-R14], (R13)	/* save user level registers, at end r13 points to ureg */
	B	_vexcep			/* call the exception handler */

TEXT _vund(SB), $-4			/* undefined */
	MOVM.IA	[R0-R3], (R13)		/* free some working space */
	MOVW	$PsrMund, R0
	B	_vswitch

TEXT _vpab(SB), $-4			/* prefetch abort */
	MOVM.IA	[R0-R3], (R13)		/* free some working space */
	MOVW	$PsrMabt, R0		/* r0 = type */
	B	_vswitch

TEXT _vdab(SB), $-4			/* data abort */
	MOVM.IA	[R0-R3], (R13)		/* free some working space */
	MOVW	$(PsrMabt+1), R0	/* r0 = type */
	B	_vswitch

TEXT _virq(SB), $-4			/* IRQ */
	MOVM.IA	[R0-R3], (R13)		/* free some working space */
	MOVW	$PsrMirq, R0		/* r0 = type */
	B	_vswitch

TEXT _vfiq(SB), $-4			/* FIQ */
	RFE				/* RIQ is special, ignore it for now */

	/*
	 *  come here with type in R0 and R13 pointing above saved [r0-r3]
	 */
_vswitch:				/* switch to svc, type in R0 */
	MOVW	SPSR, R1		/* save SPSR for ureg */
	MOVW	R14, R2			/* save interrupted pc for ureg */
	MOVW	R13, R3			/* save pointer to where the original [R0-R3] are */

	/* switch to svc mode */
	MOVW	CPSR, R14
	BIC	$PsrMask, R14
	ORR	$(PsrDirq|PsrDfiq|PsrMsvc), R14
	MOVW	R14, CPSR

	/*
	 *  R13 and R14 is now R13_SVC and R14_SVC.  The values of the previous mode's
	 *  R13 and R14 are no longer accessible.  That's why R3 was left to point to where
	 *  the old [r0-r3] are stored.
	 */
	MOVM.DB.W [R0-R2], (R13)	/* set ureg->{pc, psr, type}; r13 points to ureg->type  */
	MOVM.IA	  (R3), [R0-R3]		/* restore [R0-R3] from previous mode's stack */
	MOVM.DB.W.S [R0-R14], (R13)	/* save user level registers, at end r13 points to ureg */

	/*
	 *  if the original interrupt happened while executing SVC mode, the User R14 in the Ureg is
	 *  wrong.  We need to save the SVC one there.
	 */
	MOVW	0x40(R13), R1
	AND.S	$0xf, R1
	MOVW.NE	R14,0x38(R13)
	B	_vexcep

	/*
 	 *  call the exception routine, the ureg is at the bottom of the stack
	 */
_vexcep:
	MOVW	$setR12(SB), R12	/* Make sure we've got the kernel's SB loaded */
	MOVW	R13, R0			/* first arg is pointer to ureg */
	SUB	$8, R13			/* space for argument+link */
	BL	trap(SB)

_vrfe: 
	ADD	$(8+4*15), R13		/* make r13 point to ureg->type */
	MOVW	8(R13), R14		/* restore link */
	MOVW	4(R13), R0		/* restore SPSR */
	MOVW	R0, SPSR		/* ... */
	MOVM.DB.S (R13), [R0-R14]	/* restore registers */
	ADD	$8, R13			/* pop past ureg->{type+psr} */
	RFE				/* MOVM.IA.S.W (R13), [R15] */

TEXT splhi(SB), $-4
	MOVW	CPSR, R0
	ORR	$(PsrDfiq|PsrDirq), R0, R1
	MOVW	R1, CPSR
	RET

TEXT spllo(SB), $-4
	MOVW	CPSR, R0
	BIC	$(PsrDfiq|PsrDirq), R0, R1
	MOVW	R1, CPSR
	RET

TEXT splx(SB), $-4
	MOVW	R0, R1
	MOVW	CPSR, R0
	MOVW	R1, CPSR
	RET

TEXT splxpc(SB), $-4				/* for iunlock */
	MOVW	R0, R1
	MOVW	CPSR, R0
	MOVW	R1, CPSR
	RET

TEXT islo(SB), $-4
	MOVW	CPSR, R0
	AND	$(PsrDfiq|PsrDirq), R0
	EOR	$(PsrDfiq|PsrDirq), R0
	RET

TEXT cpsrr(SB), $-4
	MOVW	CPSR, R0
	RET

TEXT spsrr(SB), $-4
	MOVW	SPSR, R0
	RET

TEXT aamloop(SB), $-4				/* 3 */
_aamloop:
	MOVW	R0, R0				/* 1 */
	MOVW	R0, R0				/* 1 */
	MOVW	R0, R0				/* 1 */
	SUB	$1, R0				/* 1 */
	CMP	$0, R0				/* 1 */
	BNE	_aamloop			/* 3 */
	RET					/* 3 */

TEXT getcallerpc(SB), $-4
	MOVW	0(R13), R0
	RET

TEXT tas(SB), $-4
	MOVW	R0, R1
	MOVW	$0xDEADDEAD, R2
	SWPW	R2, (R1), R0
	RET

TEXT setlabel(SB), $-4
	MOVW	R13, 0(R0)			/* sp */
	MOVW	R14, 4(R0)			/* pc */
	MOVW	$0, R0
	RET

TEXT gotolabel(SB), $-4
	MOVW	0(R0), R13			/* sp */
	MOVW	4(R0), R14			/* pc */
	MOVW	$1, R0
	RET

TEXT mmuctlregr(SB), $-4
	MRC		CpMMU, 0, R0, C(CpControl), C(0)
	RET	

TEXT mmuctlregw(SB), $-4
	MCR		CpMMU, 0, R0, C(CpControl), C(0)
	MOVW		R0, R0
	MOVW		R0, R0
	RET	
