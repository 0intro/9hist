
struct Ureg
{
	ulong	status;
	ulong	cause;
	uvlong	pc;
	union{
		uvlong	sp;
		uvlong	usp;
	};
	uvlong	badvaddr;
	uvlong	tlbvirt;

	uvlong	hi;
	uvlong	lo;
	uvlong	r31;
	uvlong	r30;
	uvlong	r28;
	uvlong	r27;
	uvlong	r26;
	uvlong	r25;
	uvlong	r24;
	uvlong	r23;
	uvlong	r22;
	uvlong	r21;
	uvlong	r20;
	uvlong	r19;
	uvlong	r18;
	uvlong	r17;
	uvlong	r16;
	uvlong	r15;
	uvlong	r14;
	uvlong	r13;
	uvlong	r12;
	uvlong	r11;
	uvlong	r10;
	uvlong	r9;
	uvlong	r8;
	uvlong	r7;
	uvlong	r6;
	uvlong	r5;
	uvlong	r4;
	uvlong	r3;
	uvlong	r2;
	uvlong	r1;
};
