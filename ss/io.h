typedef struct	Duart	Duart;

#define	SYNCREG		((char*)0x40400000)
#define	DISPLAYRAM	0x1E800000
#define	EPROM		0xF6000000
#define	DUARTREG	((Duart*)0x40100000)
#define	PORT		((uchar *)0x40300000)

#define	ENAB		0x40000000	/* ASI 2, System Enable Register */
#define	ENABCACHE	0x10
#define	ENABRESET	0x04
