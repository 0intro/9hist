/*
 * Attach segment types
 */

Physseg physseg[] =
{
	{ SG_SHARED,	"shared",	0,		SEGMAXSIZE,	0, 0 },
	{ SG_BSS,	"memory",	0,		SEGMAXSIZE,	0, 0 },
	{ SG_PHYSICAL,	"eisaio",	Eisaphys,	64*1024,	0, 0 },
	{ SG_PHYSICAL,	"eisavga",	Eisavgaphys,	128*1024,	0, 0 },
	{ SG_PHYSICAL,	"bootrom",	0x1fc00000,	256*1024,	0, 0 },
	{ SG_PHYSICAL,	"nvram",	0xe0009000,	12*1024,	0, 0 },
	{ 0,		0,		0,		0,		0, 0 },
};
