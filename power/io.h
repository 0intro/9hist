#define UNCACHED	0xA0000000
#define	IO2(t,x)	((t *)(UNCACHED|0x17000000|(x)))
#define VMEA24SUP(t, x)	((t *)(UNCACHED|0x13000000|(x)))
#define	SYNCBUS(t,x)	((t *)(UNCACHED|0x1E000000|(x)))
#define	SBSEM		SYNCBUS(ulong, 0)
#define	SBSEMTOP	SYNCBUS(ulong, 0x400000)

#define	LED		((char*)0xBF200001)

typedef struct SBCC	SBCC;
typedef struct Timer	Timer;
typedef struct Duart	Duart;

struct SBCC
{
	ulong	level[14];	/* cpu interrupt level for cpu->cpu ints */
	ulong	junk0[2];
	ulong	status[14];	/* status from other cpu */
	ulong	junk1[2];
	ulong	elevel;		/* cpu interrupt level for vme->cpu ints */
	ulong	junk2[7];
	ulong	flevel;		/* cpu interrupt level for vme->cpu ints */
	ulong	junk3[3];
	ulong	overrun;
	ulong	junk4[3];
	ulong	id;		/* id of this cpu */
	ulong	eintenable;
	ulong	eintpending;
	ulong	fintenable;
	ulong	fintpending;
	ulong	idintenable;
	ulong	idintpending;
	ulong	junk5[8];
	ulong	intxmit;
};

#define	SBCCREG		SYNCBUS(SBCC, 0x400000)

#define	TIMERREG	SYNCBUS(Timer, 0x1600000)
#define	CLRTIM0		SYNCBUS(uchar, 0x1100000)
#define	CLRTIM1		SYNCBUS(uchar, 0x1180000)

#define	DUARTREG	SYNCBUS(Duart, 0x1A00000)

#define LANCERAM	IO2(uchar, 0xE00000)
#define LANCEEND	IO2(uchar, 0xF00000)
#define LANCERDP	IO2(ushort, 0xFC0002)
#define LANCERAP	IO2(ushort, 0xFC000a)
#define LANCEID		IO2(ushort, 0xFF0002)

typedef struct MODE	MODE;
typedef struct INTVEC	INTVEC;

struct MODE {
	uchar	masterslave;	/* master/slave addresses for the IO2 */
	uchar	resetforce;
	uchar	checkbits;
	uchar	promenet;
};

#define MODEREG		IO2(MODE, 0xF40000)

#define	MASTER	0x0
#define	SLAVE	0x4

struct INTVEC {
	struct {
		ulong	vec;
		ulong	fill2;
	} i[8];
};

#define INTVECREG	IO2(INTVEC, 0xF60000)
#define INTPENDREG	IO2(uchar, 0xF20000)	/* same as LED */
#define IO2CLRMASK	IO2(uchar, 0xFE0000)
#define IO2SETMASK	IO2(uchar, 0xFE8000)
#define IO2MASK		IO2(ushort, 0xFE8000)
#define	MPBERR0		IO2(ulong, 0xF48000)
#define	MPBERR1		IO2(ulong, 0xF4C000)
#define SBEADDR		((ulong *)(UNCACHED|0x1F080000))
