enum
{
	Bufsize		= 16*1024,	/* 92 ms each */
	Nbuf		= 16,		/* 1.5 seconds total */
	Dma		= 1,
	IrqAUDIO	= 5,
	SBswab		= 0,
};

#define seteisadma(a, b)	dmainit(a, Bufsize);
#define CACHELINESZ		128
#define UNCACHED(type, v)	(type*)((ulong)(v))
#define dcflush(a, b)

#define Int0vec			VectorPIC
#define setvec(v, f, a)		intrenable(v, f, a, BUSUNKNOWN)
