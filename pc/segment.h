/*
 * Attach segment types
 */

Physseg physseg[] =
{
	{ SG_SHARED,	"lock",		0,	SEGMAXSIZE,	0, 	0 },
	{ SG_SHARED,	"shared",	0,	SEGMAXSIZE,	0, 	0 },
	{ SG_PHYSICAL,	"aseg",		0xa0000, 64*1024,	0,	0 },
	{ SG_PHYSICAL,	"dseg",		0xd0000, 64*1024,	0,	0 },
	{ SG_BSS,	"memory",	0,	SEGMAXSIZE,	0,	0 },
	{ 0,		0,		0,	0,		0,	0 },
};
