/*
 *	SAC/UDA 1341 Audio driver for the Bitsy
 *
 *	The Philips UDA 1341 sound chip is accessed through the Serial Audio
 *	Controller (SAC) of the StrongARM SA-1110.  This is much more a SAC
 *	controller than a UDA controller, but we have a devsac.c already.
 *
 *	The code morphs Nicolas Pitre's <nico@cam.org> Linux controller
 *	and Ken's Soundblaster controller.
 *
 *	The interface should be identical to that of devaudio.c
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"io.h"

/*
 * GPIO based L3 bus support.
 *
 * This provides control of Philips L3 type devices. 
 * GPIO lines are used for clock, data and mode pins.
 *
 * Note: The L3 pins are shared with I2C devices. This should not present
 * any problems as long as an I2C start sequence is not generated. This is
 * defined as a 1->0 transition on the data lines when the clock is high.
 * It is critical this code only allow data transitions when the clock
 * is low. This is always legal in L3.
 *
 * The IIC interface requires the clock and data pin to be LOW when idle. We
 * must make sure we leave them in this state.
 *
 * It appears the read data is generated on the falling edge of the clock
 * and should be held stable during the clock high time.
 */

/* 
 * L3 setup and hold times (expressed in us)
 */
#define L3_DataSetupTime	1		/* 190 ns */
#define L3_DataHoldTime		1		/*  30 ns */
#define L3_ModeSetupTime	1		/* 190 ns */
#define L3_ModeHoldTime		1		/* 190 ns */
#define L3_ClockHighTime	100		/* 250 ns (min is 64*fs, 35us @ 44.1 Khz) */
#define L3_ClockLowTime		100		/* 250 ns (min is 64*fs, 35us @ 44.1 Khz) */
#define L3_HaltTime			1		/* 190 ns */

/* UDA 1341 Registers */
enum {
	/* Status0 register */
	UdaStatusDC		= 0,	/* 1 bit */
	UdaStatusIF		= 1,	/* 3 bits */
	UdaStatusSC		= 4,	/* 2 bits */
	UdaStatusRST	= 6,	/* 1 bit */
};

enum {
	/* Status1 register */
	UdaStatusPC		= 0,	/* 2 bits */
	UdaStatusDS		= 2,	/* 1 bit */
	UdaStatusPDA	= 3,	/* 1 bit */
	UdaStatusPAD	= 4,	/* 1 bit */
	UdaStatusIGS	= 5,	/* 1 bit */
	UdaStatusOGS	= 6,	/* 1 bit */
};

/*
 * UDA1341 L3 address and command types
 */
#define UDA1341_L3Addr	5
#define UDA1341_DATA0	0
#define UDA1341_DATA1	1
#define UDA1341_STATUS	2

typedef struct	AQueue	AQueue;
typedef struct	Buf	Buf;

enum
{
	Qdir		= 0,
	Qaudio,
	Qvolume,
	Qstatus,

	Fmono		= 1,
	Fin		= 2,
	Fout		= 4,

	Aclosed		= 0,
	Aread,
	Awrite,

	Vaudio		= 0,
	Vsynth,
	Vcd,
	Vline,
	Vmic,
	Vspeaker,
	Vtreb,
	Vbass,
	Vspeed,
	Nvol,

	Bufsize		= 16*1024,	/* 92 ms each */
	Nbuf		= 16,		/* 1.5 seconds total */

	Speed		= 44100,
	Ncmd		= 50,		/* max volume command words */
};

Dirtab
audiodir[] =
{
	"audio",	{Qaudio},		0,	0666,
	"volume",	{Qvolume},		0,	0666,
	"audiostat",{Qstatus},		0,	0444,
};

struct	Buf
{
	uchar*	virt;
	ulong	phys;
	Buf*	next;
};

struct	AQueue
{
	Lock;
	Buf*	first;
	Buf*	last;
};

static	struct
{
	QLock;
	Rendez	vous;
	int	bufinit;	/* boolean if buffers allocated */
	int	curcount;	/* how much data in current buffer */
	int	active;		/* boolean dma running */
	int	intr;		/* boolean an interrupt has happened */
	int	amode;		/* Aclosed/Aread/Awrite for /audio */
	int	rivol[Nvol];		/* right/left input/output volumes */
	int	livol[Nvol];
	int	rovol[Nvol];
	int	lovol[Nvol];
	int	major;		/* SB16 major version number (sb 4) */
	int	minor;		/* SB16 minor version number */
	ulong	totcount;	/* how many bytes processed since open */
	vlong	tottime;	/* time at which totcount bytes were processed */

	Buf	buf[Nbuf];	/* buffers and queues */
	AQueue	empty;
	AQueue	full;
	Buf*	current;
	Buf*	filling;
} input, output;

static	struct
{
	QLock;
	Rendez	vous;
	int	bufinit;	/* boolean if buffers allocated */
	int	curcount;	/* how much data in current buffer */
	int	active;		/* boolean dma running */
	int	intr;		/* boolean an interrupt has happened */
	int	amode;		/* Aclosed/Aread/Awrite for /audio */
	int	rivol[Nvol];		/* right/left input/output volumes */
	int	livol[Nvol];
	int	rovol[Nvol];
	int	lovol[Nvol];
	int	major;		/* SB16 major version number (sb 4) */
	int	minor;		/* SB16 minor version number */
	ulong	totcount;	/* how many bytes processed since open */
	vlong	tottime;	/* time at which totcount bytes were processed */

	Buf	buf[Nbuf];	/* buffers and queues */
	AQueue	empty;
	AQueue	full;
	Buf*	current;
	Buf*	filling;
} audio;

static	struct
{
	char*	name;
	int	flag;
	int	ilval;		/* initial values */
	int	irval;
} volumes[] =
{
[Vaudio]	"audio",	Fout, 		50,	50,
[Vsynth]	"synth",	Fin|Fout,	0,	0,
[Vcd]		"cd",		Fin|Fout,	0,	0,
[Vline]		"line",		Fin|Fout,	0,	0,
[Vmic]		"mic",		Fin|Fout|Fmono,	0,	0,
[Vspeaker]	"speaker",	Fout|Fmono,	0,	0,

[Vtreb]		"treb",		Fout, 		50,	50,
[Vbass]		"bass",		Fout, 		50,	50,

[Vspeed]	"speed",	Fin|Fout|Fmono,	Speed,	Speed,
		0
};

/*
 * Grab control of the IIC/L3 shared pins
 */
static void L3_acquirepins(void)
{
	GPSR = (GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	GPDR |=  (GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
}

/*
 * Release control of the IIC/L3 shared pins
 */
static void L3_releasepins(void)
{
	GPDR &= ~(GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	GPCR = (GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
}

/*
 * Initialize the interface
 */
static void __init L3_init(void)
{
	GAFR &= ~(GPIO_L3_SDA_io | GPIO_L3_SCLK_o | GPIO_L3_MODE_o);
	GPSR = GPIO_L3_MODE_o;
	GPDR |= GPIO_L3_MODE_o;
	L3_releasepins();
}

/*
 * Get a bit. The clock is high on entry and on exit. Data is read after
 * the clock low time has expired.
 */
static int L3_getbit(void)
{
	int data;

	GPCR = GPIO_L3_SCLK_o;
	udelay(L3_ClockLowTime);

	data = (GPLR & GPIO_L3_SDA_io) ? 1 : 0;

 	GPSR = GPIO_L3_SCLK_o;
	udelay(L3_ClockHighTime);

	return data;
}

/*
 * Send a bit. The clock is high on entry and on exit. Data is sent only
 * when the clock is low (I2C compatibility).
 */
static void L3_sendbit(int bit)
{
	GPCR = GPIO_L3_SCLK_o;

	if (bit & 1)
		GPSR = GPIO_L3_SDA_io;
	else
		GPCR = GPIO_L3_SDA_io;

	/* Assumes L3_DataSetupTime < L3_ClockLowTime */
	udelay(L3_ClockLowTime);

	GPSR = GPIO_L3_SCLK_o;
	udelay(L3_ClockHighTime);
}

/*
 * Send a byte. The mode line is set or pulsed based on the mode sequence
 * count. The mode line is high on entry and exit. The mod line is pulsed
 * before the second data byte and before ech byte thereafter.
 */
static void L3_sendbyte(char data, int mode)
{
	int i;

	L3_acquirepins();

	switch(mode) {
	    case 0: /* Address mode */
		GPCR = GPIO_L3_MODE_o;
		break;
	    case 1: /* First data byte */
		break;
	    default: /* Subsequent bytes */
		GPCR = GPIO_L3_MODE_o;
		udelay(L3_HaltTime);
		GPSR = GPIO_L3_MODE_o;
		break;
	}

	udelay(L3_ModeSetupTime);

	for (i = 0; i < 8; i++)
		L3_sendbit(data >> i);

	if (mode == 0)  /* Address mode */
		GPSR = GPIO_L3_MODE_o;

	udelay(L3_ModeHoldTime);

	L3_releasepins();
}

/*
 * Get a byte. The mode line is set or pulsed based on the mode sequence
 * count. The mode line is high on entry and exit. The mod line is pulsed
 * before the second data byte and before each byte thereafter. This
 * function is never valid with mode == 0 (address cycle) as the address
 * is always sent on the bus, not read.
 */
static char L3_getbyte(int mode)
{
	char data = 0;
	int i;

	L3_acquirepins();
	GPDR &= ~(GPIO_L3_SDA_io);

	switch(mode) {
	    case 0: /* Address mode - never valid */
		break;
	    case 1: /* First data byte */
		break;
	    default: /* Subsequent bytes */
		GPCR = GPIO_L3_MODE_o;
		udelay(L3_HaltTime);
		GPSR = GPIO_L3_MODE_o;
		break;
	}

	udelay(L3_ModeSetupTime);

	for (i = 0; i < 8; i++)
		data |= (L3_getbit() << i);

	udelay(L3_ModeHoldTime);

	L3_releasepins();

	return data;
}

/*
 * Write data to a device on the L3 bus. The address is passed as well as
 * the data and length. The length written is returned. The register space
 * is encoded in the address (low two bits are set and device address is
 * in the upper 6 bits).
 */
static int L3_write(char addr, char *data, int len)
{
	int mode = 0;
	int bytes = len;

	L3_sendbyte(addr, mode++);
	while(len--)
		L3_sendbyte(*data++, mode++);

	return bytes;
}

/*
 * Read data from a device on the L3 bus. The address is passed as well as
 * the data and length. The length read is returned. The register space
 * is encoded in the address (low two bits are set and device address is
 * in the upper 6 bits).
 */
static int L3_read(char addr, char * data, int len)
{
	int mode = 0;
	int bytes = len;

	L3_sendbyte(addr, mode++);
	while(len--)
		*data++ = L3_getbyte(mode++);

	return bytes;
}

static	char	Emode[]		= "illegal open mode";
static	char	Evolume[]	= "illegal volume specifier";

static	Buf*
getbuf(AQueue *q)
{
	Buf *b;

	ilock(q);
	b = q->first;
	if(b)
		q->first = b->next;
	iunlock(q);

	return b;
}

static	void
putbuf(AQueue *q, Buf *b)
{

	ilock(q);
	b->next = 0;
	if(q->first)
		q->last->next = b;
	else
		q->first = b;
	q->last = b;
	iunlock(q);
}

static void
audioinit(void)
{
	int err;
   
	/* Acquire and initialize DMA */
	if( audio_init_dma( &output_stream, "UDA1341 DMA out" ) ||
	    audio_init_dma( &input_stream, "UDA1341 DMA in" ) ){
		audio_clear_dma( &output_stream );
		audio_clear_dma( &input_stream );
		return -EBUSY;
	}

	L3_init();
	audio_ssp_init();

        audio_uda1341_reset();

	init_waitqueue_head(&audio_waitq);

	/* Set some default mixer values... */
	STATUS_1.DAC_gain = 1;
	STATUS_1.ADC_gain = 1;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_STATUS, (char*)&STATUS_1, 1 );
	DATA0_0.volume = 15;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_DATA0, (char*)&DATA0_0, 1 );
	DATA0_2.mode = 3;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_DATA0, (char*)&DATA0_2, 1 );
	DATA0_ext2.mixer_mode = 2;
	DATA0_ext2.mic_level = 4;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_DATA0, (char*)&DATA0_ext2, 2 );
	DATA0_ext4.AGC_ctrl = 1;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_DATA0, (char*)&DATA0_ext4, 2 );
	DATA0_ext6.AGC_level = 3;
	L3_write( (UDA1341_L3Addr<<2)|UDA1341_DATA0, (char*)&DATA0_ext6, 2 );

	/* register devices */
	audio_dev_dsp = register_sound_dsp(&UDA1341_dsp_fops, -1);
	audio_dev_mixer = register_sound_mixer(&UDA1341_mixer_fops, -1);

	printk( AUDIO_NAME_VERBOSE " initialized\n" );

	return 0;
}
