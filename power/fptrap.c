#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"
#include	"io.h"
#include	"../port/error.h"

typedef struct	FPinstr	FPinstr;
struct FPinstr
{
	ulong	op;
	ulong	load;
	ulong	store;
	ulong	branch;
	ulong	fmt;
	ulong	fs;
	ulong	fd;
	ulong	ft;
	ulong	cft;
};

enum	/* op */
{
	ABS =	5,
	ADD =	0,
	CVTD = 	65,
	CVTS =	64,
	CVTW =	68,
	DIV =	3,
	MOV =	6,
	MUL =	2,
	NEG =	7,
	SUB =	1,
};

static int	fpunimp(Ureg*, ulong, ulong);
static ulong	branch(Ureg*, ulong);

int
fptrap(Ureg *ur, ulong fcr31)
{
	ulong iw, x, npc;
	int i, ret;

	savefpregs(&u->fpsave);
	if(ur->cause & (1<<31))
		iw = *(ulong*)(ur->pc+4);
	else
		iw = *(ulong*)ur->pc;
	ret = 0;
	x = fcr31>>12;
	fcr31 &= ~(0x3F<<12);
	for(i=0; i<6; i++,x>>=1)
		if(x & 1)
			switch(i){
			case 0:	/* inexact */
pprint("inexact\n");
return 0;
			case 1: /* underflow */
pprint("underflow\n");
return 0;
			case 2:	/* overflow */
pprint("overflow\n");
return 0;
			case 3: /* division by zero */
				return 0;
			case 4: /* invalid operation */
pprint("invalid op\n");
return 0;
			case 5:	/* unimplemented operation */
				ret = fpunimp(ur, fcr31, iw);
			}
	if(ret){
		if(ur->cause & (1<<31)){
			npc = branch(ur, fcr31);
			if(npc)
				ur->pc = npc;
			else
				return 0;
		}else
			ur->pc += 4;
		restfpregs(&u->fpsave, fcr31);
	}
	return ret;
}

static int
fpdas(ulong iw, FPinstr *fp)
{
	memset(fp, ~0, sizeof(*fp));
	if((iw>>25) == 0x23){
		fp->op = iw & ((1<<5)-1);
		fp->fmt = (iw>>21) & ((1<<4)-1);
		fp->ft = (iw>>16) & ((1<<5)-1);
		fp->fs = (iw>>11) & ((1<<5)-1);
		fp->fd = (iw>>6) & ((1<<5)-1);
		fp->cft = (iw>>21) & ((1<<5)-1);
		return 1;
	}
	return 0;
}

static void
unpack(FPsave *f, int fmt, int reg, int *sign, int *exp)
{
	*sign = 1;
	if(f->fpreg[reg] & 0x80000000)
		*sign = -1;
	switch(fmt){
	case 0:
		*exp = ((f->fpreg[reg]>>23)&0xFF) - ((1<<7)-2);
		break;
	case 1:
		if(reg & 1){
			pprint("unaligned double fp register\n");
			reg &= ~1;
		}
pprint("%lux %lux\n", f->fpreg[reg], f->fpreg[reg+1]);
		*exp = ((f->fpreg[reg]>>20)&0x7FF) - ((1<<10)-2);
		break;
	}
}

static void
zeroreg(FPsave *f, int fmt, int reg, int sign)
{
	int size;

	size = 0;
	switch(fmt){
	case 0:
		size = 4;
		break;
	case 1:
		if(reg & 1)
			reg &= ~1;
		size = 8;
		break;
	}
	memset(&f->fpreg[reg], 0, size);
	if(sign < 0)
		f->fpreg[reg] |= 0x80000000;
}

static int
fpunimp(Ureg *ur, ulong fcr31, ulong iw)
{
	FPinstr instr;
	int ss, st, sd;
	int es, et, ed;
	int maxe, maxm;

pprint("fpunimp %lux %lux\n", iw, iw>>25);
	if(!fpdas(iw, &instr))
		return 0;
pprint("%d\n", instr.op);
	if(instr.op == ~0)
		return 0;
	unpack(&u->fpsave, instr.fmt, instr.fs, &ss, &es);
	unpack(&u->fpsave, instr.fmt, instr.ft, &st, &et);
	ed = 0;
	maxe = 0;
	maxm = 0;
	switch(instr.fmt){
	case 0:
		maxe = 1<<7;
		maxm = 24;
		break;
	case 1:
		maxe = 1<<10;
		maxm = 53;
		break;
	}
	switch(instr.op){
	case SUB:
		st = -st;
	case ADD:
		if(es<-(maxe-maxm) && et<-(maxe-maxm))
			ed = -maxe;
		if(es > et)
			sd = es;
		else
			sd = et;
		break;

	case DIV:
		et = -et;
	case MUL:
		sd = 1;
		if(ss != st)
			sd = -1;
		ed = es + et;
		break;
	default:
		pprint("unknown unimplemented fp op\n");
		return 0;
	}
	if(ed <= -(maxe-4)){	/* guess: underflow */
pprint("guess underflow\n");
		zeroreg(&u->fpsave, instr.fmt, instr.fd, sd);
		return 1;
	}
	return 0;
}

static ulong*
reg(Ureg *ur, int regno)
{
	/* regs go from R31 down in ureg, R29 is missing */
	if(regno == 31)
		return &ur->r31;
	if(regno == 30)
		return &ur->r30;
	if(regno == 29)
		return &ur->sp;
	return (&ur->r28) + (28-regno);
}

static ulong
branch(Ureg *ur, ulong fcr31)
{
	ulong iw, npc, rs, rt, rd, offset;

	iw = *(ulong*)ur->pc;
	rs = (iw>>21) & 0x1F;
	if(rs)
		rs = *reg(ur, rs);
	rt = (iw>>16) & 0x1F;
	if(rt)
		rt = *reg(ur, rt);
	offset = iw & ((1<<16)-1);
	if(offset & (1<<15))	/* sign extend */
		offset |= ~((1<<16)-1);
	offset <<= 2;
	/*
	 * Integer unit jumps first
	 */
	switch(iw>>26){
	case 0:			/* SPECIAL: JR or JALR */
		switch(iw&0x3F){
		case 0x09:	/* JALR */
			rd = (iw>>11) & 0x1F;
			if(rd)
				*reg(ur, rd) = ur->pc+8;
			/* fall through */
		case 0x08:	/* JR */
			return rs;
		default:
			return 0;
		}
	case 1:			/* BCOND */
		switch((iw>>16) & 0x1F){
		case 0x10:	/* BLTZAL */
			ur->r31 = ur->pc + 8;
			/* fall through */
		case 0x00:	/* BLTZ */
			if((long)rs < 0)
				return ur->pc+4 + offset;
			return ur->pc + 8;
		case 0x11:	/* BGEZAL */
			ur->r31 = ur->pc + 8;
			/* fall through */
		case 0x01:	/* BGEZ */
			if((long)rs >= 0)
				return ur->pc+4 + offset;
			return ur->pc + 8;
		default:
			return 0;
		}
	case 3:			/* JAL */
		ur->r31 = ur->pc+8;
		/* fall through */
	case 2:			/* JMP */
		npc = iw & ((1<<26)-1);
		npc <<= 2;
		return npc | (ur->pc&0xF0000000);
	case 4:			/* BEQ */
		if(rs == rt)
			return ur->pc+4 + offset;
		return ur->pc + 8;
	case 5:			/* BNE */
		if(rs != rt)
			return ur->pc+4 + offset;
		return ur->pc + 8;
	case 6:			/* BLEZ */
		if((long)rs <= 0)
			return ur->pc+4 + offset;
		return ur->pc + 8;
	case 7:			/* BGTZ */
		if((long)rs > 0)
			return ur->pc+4 + offset;
		return ur->pc + 8;
	}
	/*
	 * Floating point unit jumps
	 */
	if((iw>>26) == 0x11)	/* COP1 */
		switch((iw>>16) & 0x3C1){
		case 0x101:	/* BCT */
		case 0x181:	/* BCT */
			if(fcr31 & (1<<23))
				return ur->pc+4 + offset;
			return ur->pc + 8;
		case 0x100:	/* BCF */
		case 0x180:	/* BCF */
			if(!(fcr31 & (1<<23)))
				return ur->pc+4 + offset;
			return ur->pc + 8;
		}
	pprint("fptrap: can't do jump %lux\n", iw);
	return 0;

}
