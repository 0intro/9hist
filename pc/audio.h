enum
{
	Bufsize		= 16*1024,	/* 92 ms each */
	Nbuf		= 16,		/* 1.5 seconds total */
	Dma		= 6,
	SBswab		= 0,
};

#define seteisadma(a, b)	;
#define CACHELINESZ		8
#define UNCACHED(type, v)	(type*)((ulong)(v))
