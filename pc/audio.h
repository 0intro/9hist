enum
{
	Bufsize		= 4*1024,	/* 23.22 ms each */
	Nbuf		= 32,		/* .74 seconds total */
	Dma		= 6,
	IrqAUDIO	= 7,
	SBswab	= 0,
};

#define seteisadma(a, b)	dmainit(a, Bufsize);
#define CACHELINESZ		8
#define UNCACHED(type, v)	(type*)((ulong)(v))

#define Int0vec
#define setvec(v, f, a)		intrenable(v, f, a, BUSUNKNOWN, "audio")
