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
};

typedef struct {
	int	family;
	int	model;
	int	aalcycles;
	char*	name;
} X86type;

static X86type x86type[] =
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
	{ 6,	3,	16,	"PentiumII", },	/* determined by trial and error */
	{ 6,	5,	16,	"PentiumII", },	/* determined by trial and error */

	{ 3,	-1,	32,	"386", },	/* family defaults */
	{ 4,	-1,	22,	"486", },
	{ 5,	-1,	23,	"Pentium", },
	{ 6,	-1,	16,	"PentiumPro", },

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
	sprint(buf+i, "%s (cpuid: AX 0x%4.4luX DX 0x%4.4luX)\n",
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
	family = X86FAMILY(m->cpuidax);
	model = X86MODEL(m->cpuidax);
	for(t = x86type; t->name; t++){
		if((t->family == family && t->model == model)
		|| (t->family == family && t->model == -1)
		|| (t->family == -1))
			break;
	}
	m->cpuidtype = t->name;
	i8253init(t->aalcycles);

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

	/*
	 * Decide whether to use copy-on-reference (386 and mp).
	 */
	if(X86FAMILY(m->cpuidax) == 3 || conf.nmach > 1)
		conf.copymode = 1;

	if(X86FAMILY(m->cpuidax) == 6 /*&& conf.nmach > 1*/)
		coherence = wbflush;
}
