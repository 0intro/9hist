typedef struct	Duart	Duart;

#define	DISPLAYRAM	0x1E800000
#define	EPROM		0xF6000000
#define	CLOCK		0xF3000000
#define	CLOCKFREQ	1000000		/* one microsecond increments */
#define	KMDUART		0xF0000000	/* keyboard A, mouse B */
#define	ETHER		0xF8C00000	/* RDP, RAP */
#define	EEPROM		0xF2000000
#define	INTRREG		0xF5000000	/* interrupt register, IO space */

#define	ENAB		0x40000000	/* ASI 2, System Enable Register */
#define	ENABCACHE	0x10
#define	ENABRESET	0x04
