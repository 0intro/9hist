#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"../port/error.h"

static	int	unfinished(FPsave*);
static	int	fixq(FPsave*, int);
extern	void*	bbmalloc(int);

int
fptrap(void)
{
	int n, ret;
	ulong fsr;

	n = getfpq(&u->fpsave.q[0].a);
	if(n > NFPQ)
		panic("FPQ %d\n", n);
    again:
	fsr = getfsr();
	savefpregs(&u->fpsave);
	u->fpsave.fsr = fsr;
	ret = 0;
	switch((fsr>>14) & 7){
	case 2:
		ret = unfinished(&u->fpsave);
		break;
	}
	if(ret){
		enabfp();	/* savefpregs disables it */
		clearftt(fsr & ~0x1F);	/* clear ftt and cexc */
		restfpregs(&u->fpsave);
		enabfp();	/* restfpregs disables it */
		n = fixq(&u->fpsave, n);
		if(n > 0)
			goto again;
	}
	return ret;
}

static void
unpack(FPsave *f, int size, int reg, int *sign, int *exp)
{
	*sign = 1;
	if(f->fpreg[reg] & 0x80000000)
		*sign = -1;
	switch(size){
	case 1:
		*exp = ((f->fpreg[reg]>>23)&0xFF) - ((1<<7)-2);
		break;
	case 2:
		if(reg & 1){
			pprint("unaligned double fp register\n");
			reg &= ~1;
		}
		*exp = ((f->fpreg[reg]>>20)&0x7FF) - ((1<<10)-2);
		break;
	case 3:
		if(reg & 3){
			pprint("unaligned quad fp register\n");
			reg &= ~3;
		}
		*exp = ((f->fpreg[reg]>>16)&0x7FFF) - ((1<<14)-2);
		break;
	}
}

static void
zeroreg(FPsave *f, int size, int reg, int sign)
{
	switch(size){
	case 1:
		size = 4;
		break;
	case 2:
		if(reg & 1)
			reg &= ~1;
		size = 8;
		break;
	case 3:
		if(reg & 3)
			reg &= ~3;
		size = 16;
		break;
	}
	memset(&f->fpreg[reg], 0, size);
	if(sign < 0)
		f->fpreg[reg] |= 0x80000000;
}

static int
unfinished(FPsave *f)
{
	ulong instr;
	int size, maxe, maxm, op, rd, rs1, rs2;
	int sd, ss1, ss2;
	int ed, es1, es2;

	instr = f->q[0].i;
	if((instr&0xC1F80000) != 0x81A00000){
    bad:
		pprint("unknown unfinished instruction %lux\n", instr);
		return 0;
	}
	size = (instr>>5) & 0x3;
	if(size == 0)
		goto bad;
	maxe = 0;
	maxm = 0;
	switch(size){
	case 1:
		maxe = 1<<7;
		maxm = 24;
		break;
	case 2:
		maxe = 1<<10;
		maxm = 53;
		break;
	case 3:
		maxe = 1<<14;
		maxm = 113;
		break;
	}
	rd = (instr>>25) & 0x1F;
	rs1 = (instr>>14) & 0x1F;
	rs2 = (instr>>0) & 0x1F;
	unpack(f, size, rs1, &ss1, &es1);
	unpack(f, size, rs2, &ss2, &es2);
	op = (instr>>7) & 0x7F;
	ed = 0;
	switch(op){
	case 0x11:	/* FSUB */
		ss2 = -ss2;
	case 0x10:	/* FADD */
		if(es1<-(maxe-maxm) && es2<-(maxe-maxm))
			ed = -maxe;
		if(es1 > es2)
			sd = es1;
		else
			sd = es2;
		break;

	case 0x13:	/* FDIV */
		es2 = -es2;
	case 0x12:	/* FMUL */
		sd = 1;
		if(ss1 != ss2)
			sd = -1;
		ed = es1 + es2;
		break;

	case 0x31:	/* F?TOS */
	case 0x32:	/* F?TOD */
	case 0x33:	/* F?TOQ */
		if(es2 == maxe)		/* NaN or Inf */
			return 0;
		sd = ss2;
		ed = es2;	/* if underflow, this will do the trick */
		break;

	default:
		goto bad;
	}
	if(ed <= -(maxe-4)){	/* guess: underflow */
		zeroreg(f, size, rd, sd);
		return 1;
	}
	return 0;
}

static int
fixq(FPsave *f, int n)
{
	ulong instr, fsr;
	ulong *ip;

	while(n > 1){
		memmove(&f->q[0], &f->q[1], (n-1)*sizeof f->q[0]);
		instr = f->q[0].i;
		ip = bbmalloc(3*sizeof(ulong));
		ip[0] = instr;
		ip[1] = 0x81c3e008;	/* JMPL #8(R15), R0 [RETURN] */
		ip[2] = 0x01000000;	/* SETHI #0, R0 [NOP] */
		(*(void(*)(void))ip)();
		/*
		 * WARNING: This code is wrong (and I don't know how
		 * to fix it without emulating the entire FPU in
		 * software--please let me knw) if the queued
		 * instruction gets an exception: the getfsr() generates
		 * a trap and the staggeringly inept SPARC design
		 * translates that to a reset, as you can't have
		 * nested exceptions.  So here's the fairly solid but
		 * not fail-safe solution: jump out NOW if there's
		 * nothing else pending.  If this last instruction
		 * causes an exception, it will fire when the *user*
		 * executes the next FP instruction.  The system
		 * avoids executing any FP on the way out.  The
		 * user will trap and we'll be back but will have
		 * made progress.  The FQ will point to kernel space
		 * but the trap will happen in user space, and that's
		 * what matters.
		 *
		 * This same botch may cause the system to reset
		 * if a trap is pending when we call savefpregs() in
		 * trap.c.  I don't know; the documentation is unclear.
		 */
		if(n == 1)
			return 0;
		for(;;){
			fsr = getfsr();
			if((fsr & (1<<13)) == 0)	/* qne */
				break;
			if(fsr & 0x1F)			/* cexc */
				return n;
		}
		--n;
	}
	return 0;
}
