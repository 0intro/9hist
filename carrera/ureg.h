struct Ureg
{
	ulong	status;
	long	pc;
	union
	{
		long	sp;		/* r29 */
		long	usp;		/* r29 */
	};
	ulong	cause;
	ulong	badvaddr;
	ulong	tlbvirt;

	long	pad;	long	hi;
	long	pad;	long	lo;
	long	pad;	long	r31;
	long	pad;	long	r30;
	long	pad;	long	r28;
	long	pad;	long	r27;
	long	pad;	long	r26;
	long	pad;	long	r25;
	long	pad;	long	r24;
	long	pad;	long	r23;
	long	pad;	long	r22;
	long	pad;	long	r21;
	long	pad;	long	r20;
	long	pad;	long	r19;
	long	pad;	long	r18;
	long	pad;	long	r17;
	long	pad;	long	r16;
	long	pad;	long	r15;
	long	pad;	long	r14;
	long	pad;	long	r13;
	long	pad;	long	r12;
	long	pad;	long	r11;
	long	pad;	long	r10;
	long	pad;	long	r9;
	long	pad;	long	r8;
	long	pad;	long	r7;
	long	pad;	long	r6;
	long	pad;	long	r5;
	long	pad;	long	r4;
	long	pad;	long	r3;
	long	pad;	long	r2;
	long	pad;	long	r1;
};
