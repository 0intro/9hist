/*
 * Generic VGA registers.
 */
enum {
	MiscW		= 0x03C2,	/* Miscellaneous Output (W) */
	MiscR		= 0x03CC,	/* Miscellaneous Output (R) */
	Status0		= 0x03C2,	/* Input status 0 (R) */
	Status1		= 0x03DA,	/* Input Status 1 (R) */
	FeatureR	= 0x03CA,	/* Feature Control (R) */
	FeatureW	= 0x03DA,	/* Feature Control (W) */

	Seqx		= 0x03C4,	/* Sequencer Index, Data at Seqx+1 */
	NSeqx		= 0x05,

	Crtx		= 0x03D4,	/* CRT Controller Index, Data at Crtx+1 */
	NCrtx		= 0x19,

	Grx		= 0x03CE,	/* Graphics Controller Index, Data at Grx+1 */
	NGrax		= 0x09,

	Attrx		= 0x03C0,	/* Attribute Controller Index and Data */
	NAttrx		= 0x15,

	DACMask		= 0x03C6,	/* DAC Mask */
	DACRx		= 0x03C7,	/* DAC Read Index (W) */
	DACSts		= 0x03C7,	/* DAC Status (R) */
	DACWx		= 0x03C8,	/* DAC Write Index */
	DACData		= 0x03C9,	/* DAC Data */
	NDACx		= 0x100,
};

#define vgai(port)		inb(port)
#define vgao(port, data)	outb(port, data)

extern int vgaxi(long, uchar);
extern int vgaxo(long, uchar, uchar);

/*
 * Definitions of known hardware graphics cursors.
 */
typedef struct Hwgc {
	char	*name;
	void	(*enable)(void);
	void	(*load)(Cursor*);
	int	(*move)(Point);
	void	(*disable)(void);
} Hwgc;

extern Lock pallettelock;
