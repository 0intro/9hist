#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

static int
unimplemented(int)
{
	return 0;
}

static void
nop(void)
{
}

void (*coherence)(void) = nop;
void cycletimerinit(void);
uvlong cycletimer(uvlong*);

PCArch* arch;
extern PCArch* knownarch[];

PCArch archgeneric = {
	"generic",				/* id */
	0,					/* ident */
	i8042reset,				/* reset */
	unimplemented,				/* serialpower */
	unimplemented,				/* modempower */

	i8259init,				/* intrinit */
	i8259enable,				/* intrenable */

	i8253enable,				/* clockenable */

	i8253read,				/* read the standard timer */
};

typedef struct {
	int	family;
	int	model;
	int	aalcycles;
	char*	name;
} X86type;

static X86type x86intel[] =
{
	{ 4,	0,	22,	"486DX", },	/* known chips */
	{ 4,	1,	22,	"486DX50", },
	{ 4,	2,	22,	"486SX", },
	{ 4,	3,	22,	"486DX2", },
	{ 4,	4,	22,	"486SL", },
	{ 4,	5,	22,	"486SX2", },
	{ 4,	7,	22,	"DX2WB", },	/* P24D */
	{ 4,	8,	22,	"DX4", },	/* P24C */
	{ 4,	9,	22,	"DX4WB", },	/* P24CT */
	{ 5,	0,	23,	"P5", },
	{ 5,	1,	23,	"P5", },
	{ 5,	2,	23,	"P54C", },
	{ 5,	3,	23,	"P24T", },
	{ 5,	4,	23,	"P55C MMX", },
	{ 5,	7,	23,	"P54C VRT", },
	{ 6,	1,	16,	"PentiumPro", },/* determined by trial and error */
	{ 6,	3,	16,	"PentiumII", },
	{ 6,	5,	16,	"PentiumII/Xeon", },

	{ 3,	-1,	32,	"386", },	/* family defaults */
	{ 4,	-1,	22,	"486", },
	{ 5,	-1,	23,	"P5", },
	{ 6,	-1,	16,	"P6", },

	{ -1,	-1,	23,	"unknown", },	/* total default */
};

/*
 * The AMD processors all implement the CPUID instruction.
 * The later ones also return the processor name via functions
 * 0x80000002, 0x80000003 and 0x80000004 in registers AX, BX, CX
 * and DX:
 *	K5	"AMD-K5(tm) Processor"
 *	K6	"AMD-K6tm w/ multimedia extensions"
 *	K6 3D	"AMD-K6(tm) 3D processor"
 *	K6 3D+	?
 */
static X86type x86amd[] =
{
	{ 5,	0,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	1,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	2,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	3,	23,	"AMD-K5", },	/* guesswork */
	{ 5,	6,	11,	"AMD-K6", },	/* determined by trial and error */
	{ 5,	7,	11,	"AMD-K6", },	/* determined by trial and error */
	{ 5,	8,	11,	"AMD-K6 3D", },	/* guesswork */
	{ 5,	9,	11,	"AMD-K6 3D+", },/* guesswork */

	{ 4,	-1,	22,	"Am486", },	/* guesswork */
	{ 5,	-1,	23,	"AMD-K5/K6", },	/* guesswork */

	{ -1,	-1,	23,	"unknown", },	/* total default */
};

void
cpuidprint(void)
{
	int i;
	char buf[128];

	i = sprint(buf, "cpu%d: %dMHz ", m->machno, m->cpumhz);
	if(m->cpuidid[0])
		i += sprint(buf+i, "%s ", m->cpuidid);
	sprint(buf+i, "%s (cpuid: AX 0x%4.4uX DX 0x%4.4uX)\n",
		m->cpuidtype, m->cpuidax, m->cpuiddx);
	print(buf);
}

int
cpuidentify(void)
{
	int family, model;
	X86type *t;
	ulong cr4;
	vlong mct;

	cpuid(m->cpuidid, &m->cpuidax, &m->cpuiddx);
	if(strncmp(m->cpuidid, "AuthenticAMD", 12) == 0)
		t = x86amd;
	else
		t = x86intel;
	family = X86FAMILY(m->cpuidax);
	model = X86MODEL(m->cpuidax);
	while(t->name){
		if((t->family == family && t->model == model)
		|| (t->family == family && t->model == -1)
		|| (t->family == -1))
			break;
		t++;
	}
	m->cpuidtype = t->name;
	i8253init(t->aalcycles, t->family >= 5);

	/*
	 * If machine check exception or page size extensions are supported
	 * enable them in CR4 and clear any other set extensions.
	 * If machine check was enabled clear out any lingering status.
	 */
	if(m->cpuiddx & 0x88){
		cr4 = 0;
		if(m->cpuiddx & 0x08)
			cr4 |= 0x10;		/* page size extensions */
		if(m->cpuiddx & 0x80)
			cr4 |= 0x40;		/* machine check enable */
		putcr4(cr4);
		if(m->cpuiddx & 0x80)
			rdmsr(0x01, &mct);
	}

	return t->family;
}

void
archinit(void)
{
	PCArch **p;

	arch = 0;
	for(p = knownarch; *p; p++){
		if((*p)->ident && (*p)->ident() == 0){
			arch = *p;
			break;
		}
	}
	if(arch == 0)
		arch = &archgeneric;
	else{
		if(arch->id == 0)
			arch->id = archgeneric.id;
		if(arch->reset == 0)
			arch->reset = archgeneric.reset;
		if(arch->serialpower == 0)
			arch->serialpower = archgeneric.serialpower;
		if(arch->modempower == 0)
			arch->modempower = archgeneric.modempower;
	
		if(arch->intrinit == 0)
			arch->intrinit = archgeneric.intrinit;
		if(arch->intrenable == 0)
			arch->intrenable = archgeneric.intrenable;
	}

	/* pick the better timer */
	if(X86FAMILY(m->cpuidax) >= 5){
		cycletimerinit();
		arch->fastclock = cycletimer;
	}

	/*
	 * Decide whether to use copy-on-reference (386 and mp).
	 */
	if(X86FAMILY(m->cpuidax) == 3 || conf.nmach > 1)
		conf.copymode = 1;

	if(X86FAMILY(m->cpuidax) >= 5)
		coherence = wbflush;
}

static uvlong fasthz;

void
cycletimerinit(void)
{
	wrmsr(0x10, 0);
	fasthz = m->cpuhz;
}

/*
 *  return the most precise clock we have
 */
uvlong
cycletimer(uvlong *hz)
{
	uvlong tsc;

	rdmsr(0x10, (vlong*)&tsc);
	m->fastclock = tsc;
	if(hz != nil)
		*hz = fasthz;
	return tsc;
}

vlong
fastticks(uvlong *hz)
{
	return (*arch->fastclock)(hz);
}
