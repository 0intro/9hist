#define	DISPLAYRAM	0x1E800000
#define	EPROM		0xF6000000
#define	CLOCK		0xF3000000
#define	CLOCKFREQ	1000000		/* one microsecond increments */
#define	KMDUART		0xF0000000	/* keyboard A, mouse B */
#define	EIADUART	0xF1000000	/* serial ports */
#define	ETHER		0xF8C00000	/* RDP, RAP */
#define	EEPROM		0xF2000000
#define	INTRREG		0xF5000000	/* interrupt register, IO space */

typedef struct SCCdev	SCCdev;
struct SCCdev
{
	uchar	ptrb;
	uchar	dummy1;
	uchar	datab;
	uchar	dummy2;
	uchar	ptra;
	uchar	dummy3;
	uchar	dataa;
	uchar	dummy4;
};

#define KMFREQ	10000000	/* crystal frequency for kbd/mouse 8530 */
#define EIAFREQ	5000000		/* crystal frequency for serial port 8530 */
