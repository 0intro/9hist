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
#include	"sa1110dma.h"

static int debug = 0;

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
 * L3 setup and hold times (expressed in µs)
 */
enum {
	L3_AcquireTime =		1,
	L3_ReleaseTime =		1,
	L3_DataSetupTime =	(190+999)/1000,	/* 190 ns */
	L3_DataHoldTime =		( 30+999)/1000,
	L3_ModeSetupTime =	(190+999)/1000,
	L3_ModeHoldTime =	(190+999)/1000,
	L3_ClockHighTime =	(100+999)/1000,
	L3_ClockLowTime =		(100+999)/1000,
	L3_HaltTime =			(190+999)/1000,
};

/* UDA 1341 Registers */
enum {
	/* Status0 register */
	UdaStatusDC		= 0,	/* 1 bit */
	UdaStatusIF		= 1,	/* 3 bits */
	UdaStatusSC		= 4,	/* 2 bits */
	UdaStatusRST		= 6,	/* 1 bit */
};

enum {
	/* Status1 register */
	UdaStatusPC	= 0,	/* 2 bits */
	UdaStatusDS	= 2,	/* 1 bit */
	UdaStatusPDA	= 3,	/* 1 bit */
	UdaStatusPAD	= 4,	/* 1 bit */
	UdaStatusIGS	= 5,	/* 1 bit */
	UdaStatusOGS	= 6,	/* 1 bit */
};

/*
 * UDA1341 L3 address and command types
 */

enum {
	UDA1341_DATA0 =	0,
	UDA1341_DATA1,
	UDA1341_STATUS,
	UDA1341_L3Addr = 0x14,
};

typedef struct	AQueue	AQueue;
typedef struct	Buf	Buf;
typedef struct	IOstate IOstate;

enum
{
	Qdir		= 0,
	Qaudio,
	Qvolume,
	Qspeed,
	Qstatus,

	Fmono		= 1,
	Fin			= 2,
	Fout			= 4,

	Aclosed		= 0,
	Aread,
	Awrite,

	Vaudio		= 0,
	Vmic,
	Vtreb,
	Vbass,
	Vspeed,
	Vfilter,
	Vinvert,
	Nvol,

	Bufsize		= 4* 1024,	/* 23 ms each */
	Nbuf			= 10,			/* 230 ms total */

	Speed		= 44100,
	Ncmd		= 50,			/* max volume command words */
};

enum {
	Flushbuf = 0xe0000000,
};

/* System Clock -- according to the manual, it seems that when the UDA is
    configured in non MSB/I2S mode, it uses a divisor of 256 to the 12.288MHz
    clock.  The other rates are only supported in MSB mode, which should be 
    implemented at some point */
enum {
	SC512FS = 0 << 2,
	SC384FS = 1 << 2,
	SC256FS = 2 << 2,
};

/* Format */
enum {
	LSB16 =	1 << 1,
	LSB18 =	2 << 1,
	LSB20 =	3 << 1,
	MSB =	4 << 1,
	MSB16 =	5 << 1,
	MSB18 =	6 << 1,
	MSB20 =	7 << 1,
};

Dirtab
audiodir[] =
{
	".",		{Qdir, 0, QTDIR},	0,	DMDIR|0555,
	"audio",	{Qaudio},			0,	0666,
	"volume",	{Qvolume},		0,	0666,
	"speed",	{Qspeed},			0,	0666,
	"audiostat",{Qstatus},		0,	0444,
};

struct	Buf
{
	uchar*	virt;
	ulong	phys;
	uint	nbytes;
};

struct	IOstate
{
	QLock;
	Lock			ilock;
	Rendez		vous;
	Chan		*chan;		/* chan of open */
	int			dma;			/* dma chan, alloc on open, free on close */
	int			bufinit;		/* boolean, if buffers allocated */
	Buf			buf[Nbuf];		/* buffers and queues */
	volatile Buf	*current;		/* next dma to finish */
	volatile Buf	*next;		/* next candidate for dma */
	volatile Buf	*filling;		/* buffer being filled */
/* to have defines just like linux — there's a real operating system */
#define emptying filling
};

static	struct
{
	QLock;
	int		amode;			/* Aclosed/Aread/Awrite for /audio */
	int		intr;			/* boolean an interrupt has happened */
	int		rivol[Nvol];	/* right/left input/output volumes */
	int		livol[Nvol];
	int		rovol[Nvol];
	int		lovol[Nvol];
	ulong	totcount;		/* how many bytes processed since open */
	vlong	tottime;		/* time at which totcount bytes were processed */
	IOstate	i;
	IOstate	o;
} audio;

int	zerodma;	/* dma buffer used for sending zero */

static struct
{
	ulong	bytes;
	ulong	totaldma;
	ulong	idledma;
	ulong	faildma;
	ulong	samedma;
	ulong	empties;
} iostats;

static	struct
{
	char*	name;
	int	flag;
	int	ilval;		/* initial values */
	int	irval;
} volumes[] =
{
[Vaudio]	{"audio",	Fout|Fmono,	 	80,		80},
[Vmic]	{"mic",	Fin|Fmono,		  0,		  0},
[Vtreb]	{"treb",	Fout|Fmono,		50,		50},
[Vbass]	{"bass",	Fout|Fmono, 		50,		50},
[Vspeed]	{"speed",	Fin|Fout|Fmono,	Speed,	Speed},
[Vfilter]	{"filter",	Fout|Fmono,		  0,		  0},
[Vinvert]	{"invert",	Fin|Fout|Fmono,	  0,		  0},
[Nvol]	{0}
};
static int rate = Speed;		/* Current sample rate */
static int bufsize = Bufsize;	/* Current buffer size */
static void	setreg(char *name, int val, int n);

/*
 * Grab control of the IIC/L3 shared pins
 */
static void
L3_acquirepins(void)
{
	gpioregs->set = (GPIO_L3_MODE_o | GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	gpioregs->direction |=  (GPIO_L3_MODE_o | GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	µdelay(L3_AcquireTime);
}

/*
 * Release control of the IIC/L3 shared pins
 */
static void
L3_releasepins(void)
{
	gpioregs->direction &= ~(GPIO_L3_MODE_o | GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	µdelay(L3_ReleaseTime);
}

/*
 * Initialize the interface
 */
static void 
L3_init(void)
{
	gpioregs->altfunc &= ~(GPIO_L3_SDA_io | GPIO_L3_SCLK_o | GPIO_L3_MODE_o);
	L3_releasepins();
}

/*
 * Get a bit. The clock is high on entry and on exit. Data is read after
 * the clock low time has expired.
 */
static int
L3_getbit(void)
{
	int data;

	gpioregs->clear = GPIO_L3_SCLK_o;
	µdelay(L3_ClockLowTime);

	data = (gpioregs->level & GPIO_L3_SDA_io) ? 1 : 0;

 	gpioregs->set = GPIO_L3_SCLK_o;
	µdelay(L3_ClockHighTime);

	return data;
}

/*
 * Send a bit. The clock is high on entry and on exit. Data is sent only
 * when the clock is low (I2C compatibility).
 */
static void
L3_sendbit(int bit)
{
	gpioregs->clear = GPIO_L3_SCLK_o;

	if (bit & 1)
		gpioregs->set = GPIO_L3_SDA_io;
	else
		gpioregs->clear = GPIO_L3_SDA_io;

	/* Assumes L3_DataSetupTime < L3_ClockLowTime */
	µdelay(L3_ClockLowTime);

	gpioregs->set = GPIO_L3_SCLK_o;
	µdelay(L3_ClockHighTime);
}

/*
 * Send a byte. The mode line is set or pulsed based on the mode sequence
 * count. The mode line is high on entry and exit. The mod line is pulsed
 * before the second data byte and before ech byte thereafter.
 */
static void
L3_sendbyte(char data, int mode)
{
	int i;

	switch(mode) {
	case 0: /* Address mode */
		gpioregs->clear = GPIO_L3_MODE_o;
		break;
	case 1: /* First data byte */
		break;
	default: /* Subsequent bytes */
		gpioregs->clear = GPIO_L3_MODE_o;
		µdelay(L3_HaltTime);
		gpioregs->set = GPIO_L3_MODE_o;
		break;
	}

	µdelay(L3_ModeSetupTime);

	for (i = 0; i < 8; i++)
		L3_sendbit(data >> i);

	if (mode == 0)  /* Address mode */
		gpioregs->set = GPIO_L3_MODE_o;

	µdelay(L3_ModeHoldTime);
}

/*
 * Get a byte. The mode line is set or pulsed based on the mode sequence
 * count. The mode line is high on entry and exit. The mod line is pulsed
 * before the second data byte and before each byte thereafter. This
 * function is never valid with mode == 0 (address cycle) as the address
 * is always sent on the bus, not read.
 */
static char
L3_getbyte(int mode)
{
	char data = 0;
	int i;

	switch(mode) {
	case 0: /* Address mode - never valid */
		break;
	case 1: /* First data byte */
		break;
	default: /* Subsequent bytes */
		gpioregs->clear = GPIO_L3_MODE_o;
		µdelay(L3_HaltTime);
		gpioregs->set = GPIO_L3_MODE_o;
		break;
	}

	µdelay(L3_ModeSetupTime);

	for (i = 0; i < 8; i++)
		data |= (L3_getbit() << i);

	µdelay(L3_ModeHoldTime);

	return data;
}

/*
 * Write data to a device on the L3 bus. The address is passed as well as
 * the data and length. The length written is returned. The register space
 * is encoded in the address (low two bits are set and device address is
 * in the upper 6 bits).
 */
static int
L3_write(uchar addr, uchar *data, int len)
{
	int mode = 0;
	int bytes = len;

	L3_acquirepins();
	L3_sendbyte(addr, mode++);
	while(len--)
		L3_sendbyte(*data++, mode++);
	L3_releasepins();
	return bytes;
}

/*
 * Read data from a device on the L3 bus. The address is passed as well as
 * the data and length. The length read is returned. The register space
 * is encoded in the address (low two bits are set and device address is
 * in the upper 6 bits).

 * Commented out, not used
static int
L3_read(uchar addr, uchar *data, int len)
{
	int mode = 0;
	int bytes = len;

	L3_acquirepins();
	L3_sendbyte(addr, mode++);
	gpioregs->direction &= ~(GPIO_L3_SDA_io);
	while(len--)
		*data++ = L3_getbyte(mode++);
	L3_releasepins();
	return bytes;
}
 */

void
audiomute(int on)
{
	egpiobits(EGPIO_audio_mute, on);
}

static	char	Emode[]		= "illegal open mode";
static	char	Evolume[]	= "illegal volume specifier";

static void
bufinit(IOstate *b)
{
	int i;

	if (debug) print("bufinit\n");
	for (i = 0; i < Nbuf; i++) {
		b->buf[i].virt = xalloc(Bufsize);
		b->buf[i].phys = PADDR(b->buf[i].virt);
		memset(b->buf[i].virt, 0xAA, Bufsize);
	}
	b->bufinit = 1;
};

static void
setempty(IOstate *b)
{
	int i;

	if (debug) print("setempty\n");
	for (i = 0; i < Nbuf; i++) {
		b->buf[i].nbytes = 0;
	}
	b->filling = b->buf;
	b->current = b->buf;
	b->next = b->buf;
}

static int
audioqnotempty(void *x)
{
	IOstate *s = x;

	return dmaidle(s->dma) || s->emptying != s->current;
}

static int
audioqnotfull(void *x)
{
	IOstate *s = x;

	return dmaidle(s->dma) || s->filling != s->current;
}

SSPregs *sspregs;
MCPregs	*mcpregs;

static void
audioinit(void)
{
	/* Turn MCP operations off */
	mcpregs = mapspecial(MCPREGS, 0x34);
	mcpregs->status &= ~(1<<16);

	sspregs = mapspecial(SSPREGS, 32);

}

uchar	status0[1]		= {0x02};
uchar	status1[1]		= {0x80};
uchar	data00[1]		= {0x00};		/* volume control, bits 0 – 5 */
uchar	data01[1]		= {0x40};
uchar	data02[1]		= {0x80};
uchar	data0e0[2]	= {0xc0, 0xe0};
uchar	data0e1[2]	= {0xc1, 0xe0};
uchar	data0e2[2]	= {0xc2, 0xf2};
/* there is no data0e3 */
uchar	data0e4[2]	= {0xc4, 0xe0};
uchar	data0e5[2]	= {0xc5, 0xe0};
uchar	data0e6[2]	= {0xc6, 0xe3};

static void
enable(void)
{
	uchar	data[1], clock;

	L3_init();

	/* Setup the uarts */
	ppcregs->assignment &= ~(1<<18);

	sspregs->control0 = 0;
	sspregs->control0 = 0x031f; /* 16 bits, TI frames, serial clock rate 3 */
	sspregs->control1 = 0x0020; /* ext clock */
	sspregs->control0 = 0x039f;	/* enable */

	/* Enable the audio power */
	egpiobits(EGPIO_audio_ic_power | EGPIO_codec_reset, 1);

	/* external clock configured for 44100 samples/sec */
	switch (rate) {
	case 32000:
	case 48000:
		/* 00 */
		gpioregs->clear = GPIO_CLK_SET0_o|GPIO_CLK_SET1_o;
		break;
	default:
		rate = 44100;
	case 44100:
		/* 01 */
		gpioregs->set = GPIO_CLK_SET0_o;
		gpioregs->clear = GPIO_CLK_SET1_o;
		break;
	case 8000:
	case 16000:
		/* 10 */
		gpioregs->set = GPIO_CLK_SET1_o;
		gpioregs->clear = GPIO_CLK_SET0_o;
		break;
	case 11025:
	case 22050:
		/* 11 */
		gpioregs->set = GPIO_CLK_SET0_o|GPIO_CLK_SET1_o;
		break;
	}

	/* Wait for the UDA1341 to wake up */
	delay(100);

	/* Reset the chip */
	switch (rate) {
	case 8000:
	case 11025:
		clock = SC512FS;	/* Only works in MSB mode! */
		break;
	default:
	case 16000:
	case 22050:
	case 44100:
	case 48000:
		clock = SC256FS;
		break;
	case 32000:
		clock = SC384FS;	/* Only works in MSB mode! */
		break;
	}
	data[0] = status0[0] | 1<<UdaStatusRST | clock;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, data, 1 );
	if (debug)
		print("enable:	status0	= 0x%2.2ux\n", data[0]);

	gpioregs->clear = EGPIO_codec_reset;
	gpioregs->set = EGPIO_codec_reset;
	/* write uda 1341 status[0] */
	data[0] = status0[0] | clock;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, data, 1);
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
	L3_write(UDA1341_L3Addr | UDA1341_DATA0, data02, 1);
	L3_write(UDA1341_L3Addr | UDA1341_DATA0, data0e2, 2);
	L3_write(UDA1341_L3Addr | UDA1341_DATA0, data0e6, 2 );

	if (debug) {
		print("enable:	status0	= 0x%2.2ux\n", data[0]);
		print("enable:	status1	= 0x%2.2ux\n", status1[0]);
		print("enable:	data02	= 0x%2.2ux\n", data02[0]);
		print("enable:	data0e2	= 0x%4.4ux\n", data0e2[0] | data0e2[1]<<8);
		print("enable:	data0e4	= 0x%4.4ux\n", data0e4[0] | data0e4[1]<<8);
		print("enable:	data0e6	= 0x%4.4ux\n", data0e6[0] | data0e6[1]<<8);
		print("enable:	sspregs->control0 = 0x%lux\n", sspregs->control0);
		print("enable:	sspregs->control1 = 0x%lux\n", sspregs->control1);
	}
}

static	void
resetlevel(void)
{
	int i;

	for(i=0; volumes[i].name; i++) {
		audio.lovol[i] = volumes[i].ilval;
		audio.rovol[i] = volumes[i].irval;
		audio.livol[i] = volumes[i].ilval;
		audio.rivol[i] = volumes[i].irval;
	}
}

static void
mxvolume(void) {
	int *left, *right;

	if(audio.amode & Aread){
		left = audio.livol;
		right = audio.rivol;
		if (left[Vmic]+right[Vmic] == 0) {
			/* Turn on automatic gain control (AGC) */
			data0e4[1] |= 0x10;
			L3_write(UDA1341_L3Addr | UDA1341_DATA0, data0e4, 2 );
		} else {
			int v;
			/* Turn on manual gain control */
			v = ((left[Vmic]+right[Vmic])*0x7f/200)&0x7f;
			data0e4[1] &= ~0x13;
			data0e5[1] &= ~0x1f;
			data0e4[1] |= v & 0x3;
			data0e5[0] |= (v & 0x7c)<<6;
			data0e5[1] |= (v & 0x7c)>>2;
			L3_write(UDA1341_L3Addr | UDA1341_DATA0, data0e4, 2 );
			L3_write(UDA1341_L3Addr | UDA1341_DATA0, data0e5, 2 );
		}
		if (left[Vinvert]+right[Vinvert] == 0)
			status1[0] &= ~0x04;
		else
			status1[0] |= 0x04;
		L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
		if (debug) {
			print("mxvolume:	status1	= 0x%2.2ux\n", status1[0]);
			print("mxvolume:	data0e4	= 0x%4.4ux\n", data0e4[0]|data0e4[0]<<8);
			print("mxvolume:	data0e5	= 0x%4.4ux\n", data0e5[0]|data0e5[0]<<8);
		}
	}
	if(audio.amode & Awrite){
		left = audio.lovol;
		right = audio.rovol;
		data00[0] &= ~0x3f;
		data00[0] |= ((200-left[Vaudio]-right[Vaudio])*0x3f/200)&0x3f;
		if (left[Vtreb]+right[Vtreb] <= 100
		 && left[Vbass]+right[Vbass] <= 100)
			/* settings neutral */
			data02[0] &= ~0x03;
		else {
			data02[0] |= 0x03;
			data01[0] &= ~0x3f;
			data01[0] |= ((left[Vtreb]+right[Vtreb]-100)*0x3/100)&0x03;
			data01[0] |= (((left[Vbass]+right[Vbass]-100)*0xf/100)&0xf)<<2;
		}
		if (left[Vfilter]+right[Vfilter] == 0)
			data02[0] &= ~0x10;
		else
			data02[0]|= 0x10;
		if (left[Vinvert]+right[Vinvert] == 0)
			status1[0] &= ~0x10;
		else
			status1[0] |= 0x10;
		L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
		L3_write(UDA1341_L3Addr | UDA1341_DATA0, data00, 1);
		L3_write(UDA1341_L3Addr | UDA1341_DATA0, data01, 1);
		L3_write(UDA1341_L3Addr | UDA1341_DATA0, data02, 1);
		if (debug) {
			print("mxvolume:	status1	= 0x%2.2ux\n", status1[0]);
			print("mxvolume:	data00	= 0x%2.2ux\n", data00[0]);
			print("mxvolume:	data01	= 0x%2.2ux\n", data01[0]);
			print("mxvolume:	data02	= 0x%2.2ux\n", data02[0]);
		}
	}
}

static void
setreg(char *name, int val, int n)
{
	uchar x[2];
	int i;

	x[0] = val;
	x[1] = val>>8;

	if(strcmp(name, "pause") == 0){
		for(i = 0; i < n; i++)
			µdelay(val);
		return;
	}

	switch(n){
	case 1:
	case 2:
		break;
	default:
		error("setreg");
	}

	if(strcmp(name, "status") == 0){
		L3_write(UDA1341_L3Addr | UDA1341_STATUS, x, n);
	} else if(strcmp(name, "data0") == 0){
		L3_write(UDA1341_L3Addr | UDA1341_DATA0, x, n);
	} else if(strcmp(name, "data1") == 0){
		L3_write(UDA1341_L3Addr | UDA1341_DATA1, x, n);
	} else
		error("setreg");
}

static void
outenable(void) {
	/* turn on DAC, set output gain switch */
	egpiobits(EGPIO_audio_power, 1);
	audiomute(0);
	status1[0] |= 0x41;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
	/* set volume */
	data00[0] |= 0xf;
	L3_write(UDA1341_L3Addr | UDA1341_DATA0, data00, 1);
	if (debug) {
		print("outenable:	status1	= 0x%2.2ux\n", status1[0]);
		print("outenable:	data00	= 0x%2.2ux\n", data00[0]);
	}
}

static void
outdisable(void) {
	dmastop(audio.o.dma);
	/* turn off DAC, clear output gain switch */
	audiomute(1);
	egpiobits(EGPIO_audio_power, 0);
	status1[0] &= ~0x41;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
	if (debug) {
		print("outdisable:	status1	= 0x%2.2ux\n", status1[0]);
	}
	egpiobits(EGPIO_audio_power, 0);
}

static void
inenable(void) {
	/* turn on ADC, set input gain switch */
	status1[0] |= 0x22;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
	if (debug) {
		print("inenable:	status1	= 0x%2.2ux\n", status1[0]);
	}
}

static void
indisable(void) {
	dmastop(audio.i.dma);
	/* turn off ADC, clear input gain switch */
	status1[0] &= ~0x22;
	L3_write(UDA1341_L3Addr | UDA1341_STATUS, status1, 1);
	if (debug) {
		print("indisable:	status1	= 0x%2.2ux\n", status1[0]);
	}
}

static void
sendaudio(IOstate *s) {
	/* interrupt routine calls this too */
	int n;

	if (debug > 1) print("#A: sendaudio\n");
	ilock(&s->ilock);
	if ((audio.amode &  Aread) && s->next == s->filling && dmaidle(s->dma)) {
		// send an empty buffer to provide an input clock
		zerodma |= dmastart(s->dma, Flushbuf, bufsize) & 0xff;
		if (zerodma == 0)
			if (debug) print("emptyfail\n");
		iostats.empties++;
		iunlock(&s->ilock);
		return;
	}
	while (s->next != s->filling) {
		s->next->nbytes &= ~0x3;	/* must be a multiple of 4 */
		if(s->next->nbytes) {
			if ((n = dmastart(s->dma, s->next->phys, s->next->nbytes)) == 0) {
				iostats.faildma++;
				break;
			}
			iostats.totaldma++;
			switch (n >> 8) {
			case 1:
				iostats.idledma++;
				break;
			case 3:
				iostats.faildma++;
				break;
			}
			if (debug) {
				if (debug > 1)
					print("dmastart @%p\n", s->next);
				else
					iprint("+");
			}
			s->next->nbytes = 0;
		}
		s->next++;
		if (s->next == &s->buf[Nbuf])
			s->next = &s->buf[0];
	}
	iunlock(&s->ilock);
}

static void
recvaudio(IOstate *s) {
	/* interrupt routine calls this too */
	int n;

	if (debug > 1) print("#A: recvaudio\n");
	ilock(&s->ilock);
	while (s->next != s->emptying) {
		assert(s->next->nbytes == 0);
		if ((n = dmastart(s->dma, s->next->phys, bufsize)) == 0) {
			iostats.faildma++;
			break;
		}
		iostats.totaldma++;
		switch (n >> 8) {
		case 1:
			iostats.idledma++;
			break;
		case 3:
			iostats.faildma++;
			break;
		}
		if (debug) {
			if (debug > 1)
				print("dmastart @%p\n", s->next);
			else
				iprint("+");
		}
		s->next++;
		if (s->next == &s->buf[Nbuf])
			s->next = &s->buf[0];
	}
	iunlock(&s->ilock);
}

void
audiopower(int flag) {
	IOstate *s;

	if (debug) {
		iprint("audiopower %d\n", flag);
	}
	if (flag) {
		/* power on only when necessary */
		if (audio.amode) {
			egpiobits(EGPIO_audio_power | EGPIO_audio_ic_power | EGPIO_codec_reset, 1);
			enable();
			if (audio.amode & Aread) {
				inenable();
				s = &audio.i;
				dmareset(s->dma, 1, 0, 4, 2, SSPRecvDMA, Port4SSP);
				recvaudio(s);
			}
			if (audio.amode & Awrite) {
				outenable();
				s = &audio.o;
				dmareset(s->dma, 0, 0, 4, 2, SSPXmitDMA, Port4SSP);
				sendaudio(s);
			}
			mxvolume();
		}
	} else {
		/* power off */
		if (audio.amode & Aread)
			indisable();
		if (audio.amode & Awrite)
			outdisable();
		egpiobits(EGPIO_audio_ic_power | EGPIO_codec_reset | EGPIO_audio_power, 0);
	}
}

static void
audiointr(void *x, ulong ndma) {
	IOstate *s = x;

	if (debug) {
		if (debug > 1)
			iprint("#A: audio interrupt @%p\n", s->current);
		else
			iprint("-");
	}
	if (s == &audio.i || (ndma & ~zerodma)) {
		/* A dma, not of a zero buffer completed, update current
		 * Only interrupt routine touches s->current
		 */
		s->current->nbytes = (s == &audio.i)? bufsize: 0;
		s->current++;
		if (s->current == &s->buf[Nbuf])
			s->current = &s->buf[0];
	}
	if (ndma) {
		if (s == &audio.o) {
			zerodma &= ~ndma;
			sendaudio(s);
		} else if (s == &audio.i)
			recvaudio(s);
	}
	wakeup(&s->vous);
}

static Chan*
audioattach(char *param)
{
	return devattach('A', param);
}

static Walkqid*
audiowalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, audiodir, nelem(audiodir), devgen);
}

static int
audiostat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, audiodir, nelem(audiodir), devgen);
}

static Chan*
audioopen(Chan *c, int mode)
{
	IOstate *s;
	int omode = mode;

	switch((ulong)c->qid.path) {
	default:
		error(Eperm);
		break;

	case Qstatus:
		if((omode&7) != OREAD)
			error(Eperm);
	case Qvolume:
	case Qspeed:
	case Qdir:
		break;

	case Qaudio:
		omode = (omode & 0x7) + 1;
		if (omode & ~(Aread | Awrite))
			error(Ebadarg);
		qlock(&audio);
		if(audio.amode & omode){
			qunlock(&audio);
			error(Einuse);
		}
		enable();
		memset(&iostats, 0, sizeof(iostats));
		if (omode & Aread) {
			inenable();
			s = &audio.i;
			if(s->bufinit == 0)
				bufinit(s);
			setempty(s);
			s->emptying = &s->buf[Nbuf-1];
			s->chan = c;
			s->dma = dmaalloc(1, 0, 4, 2, SSPRecvDMA, Port4SSP, audiointr, (void*)s);
			audio.amode |= Aread;
		}
		if (omode & (Aread|Awrite) && (audio.amode & Awrite) == 0) {
			s = &audio.o;
			if(s->bufinit == 0)
				bufinit(s);
			setempty(s);
			s->chan = c;
			s->dma = dmaalloc(0, 0, 4, 2, SSPXmitDMA, Port4SSP, audiointr, (void*)s);
		}	
		if (omode & Awrite) {
			audio.amode |= Awrite;
			outenable();
		}
		mxvolume();
		qunlock(&audio);
		if (audio.amode & Aread)
			sendaudio(&audio.o);
			
		if (debug) print("open done\n");
		break;
	}
	c = devopen(c, mode, audiodir, nelem(audiodir), devgen);
	c->mode = openmode(mode);
	c->flag |= COPEN;
	c->offset = 0;

	return c;
}

static void
audioclose(Chan *c)
{
	IOstate *s;

	switch((ulong)c->qid.path) {
	default:
		error(Eperm);
		break;

	case Qdir:
	case Qvolume:
	case Qspeed:
	case Qstatus:
		break;

	case Qaudio:
		if (debug > 1) print("#A: close\n");
		if(c->flag & COPEN) {
			qlock(&audio);
			if (audio.i.chan == c) {
				/* closing the read end */
				audio.amode &= ~Aread;
				s = &audio.i;
				qlock(s);
				indisable();
				setempty(s);
				dmafree(s->dma);
				qunlock(s);
				if ((audio.amode & Awrite) == 0) {
					s = &audio.o;
					qlock(s);
					while(waserror())
						;
					dmawait(s->dma);
					poperror();
					outdisable();
					setempty(s);
					dmafree(s->dma);
					qunlock(s);
				}
			}
			if (audio.o.chan == c) {
				/* closing the write end */
				audio.amode &= ~Awrite;
				s = &audio.o;
				qlock(s);
				if (s->filling->nbytes) {
					/* send remaining partial buffer */
					s->filling++;
					if (s->filling == &s->buf[Nbuf])
						s->filling = &s->buf[0];
					sendaudio(s);
				}
				while(waserror())
					;
				dmawait(s->dma);
				poperror();
				outdisable();
				setempty(s);
				if ((audio.amode & Aread) == 0)
					dmafree(s->dma);
				qunlock(s);
			}
			if (audio.amode == 0) {
				/* turn audio off */
				egpiobits(EGPIO_audio_ic_power | EGPIO_codec_reset, 0);
			}
			qunlock(&audio);
			if (debug) {
				print("total dmas: %lud\n", iostats.totaldma);
				print("dmas while idle: %lud\n", iostats.idledma);
				print("dmas while busy: %lud\n", iostats.faildma);
				print("out of order dma: %lud\n", iostats.samedma);
			}
		}
		break;
	}
}

static long
audioread(Chan *c, void *v, long n, vlong off)
{
	int liv, riv, lov, rov, nsendb, nbufs;
	long m, n0;
	char buf[300];
	int j;
	ulong offset = off;
	char *p;
	IOstate *s;

	n0 = n;
	p = v;
	switch((ulong)c->qid.path) {
	default:
		error(Eperm);
		break;

	case Qdir:
		return devdirread(c, p, n, audiodir, nelem(audiodir), devgen);

	case Qaudio:
		if (debug > 1) print("#A: read %ld\n", n);
		if((audio.amode & Aread) == 0)
			error(Emode);
		s = &audio.i;
		qlock(s);
		if(waserror()){
			qunlock(s);
			nexterror();
		}
		while(n > 0) {
			if(s->emptying->nbytes == 0) {
				if (debug > 1) print("#A: emptied @%p\n", s->emptying);
				recvaudio(s);
				s->emptying++;
				if (s->emptying == &s->buf[Nbuf])
					s->emptying = s->buf;
			}
			/* wait if dma in progress */
			while (!dmaidle(s->dma) && s->emptying == s->current) {
				if (debug > 1) print("#A: sleep\n");
				sleep(&s->vous, audioqnotempty, s);
			}

			m = (s->emptying->nbytes > n)? n: s->emptying->nbytes;
			memmove(p, s->emptying->virt + bufsize - 
					  s->emptying->nbytes, m);

			s->emptying->nbytes -= m;
			n -= m;
			p += m;
		}
		poperror();
		qunlock(s);
		break;
		break;

	case Qstatus:
		buf[0] = 0;
		snprint(buf, sizeof(buf), "bytes %lud\ntime %lld\n",
			audio.totcount, audio.tottime);
		return readstr(offset, p, n, buf);

	case Qspeed:
		s = &audio.o;
		nbufs = s->filling - s->current;
		if (nbufs < 0) nbufs = Nbuf + nbufs;
		nsendb = (nbufs - 1) * bufsize + s->current->nbytes;
		buf[0] = '\0';
		snprint(buf, sizeof(buf), "speed %d\ndmasize %d\nsendbytes %d\n", rate, bufsize, nsendb);
		return readstr(offset, p, n, buf);

	case Qvolume:
		j = 0;
		buf[0] = 0;
		for(m=0; volumes[m].name; m++){
			liv = audio.livol[m];
			riv = audio.rivol[m];
			lov = audio.lovol[m];
			rov = audio.rovol[m];
			j += snprint(buf+j, sizeof(buf)-j, "%s", volumes[m].name);
			if((volumes[m].flag & Fmono) || liv==riv && lov==rov){
				if((volumes[m].flag&(Fin|Fout))==(Fin|Fout) && liv==lov)
					j += snprint(buf+j, sizeof(buf)-j, " %d", liv);
				else{
					if(volumes[m].flag & Fin)
						j += snprint(buf+j, sizeof(buf)-j,
							" in %d", liv);
					if(volumes[m].flag & Fout)
						j += snprint(buf+j, sizeof(buf)-j,
							" out %d", lov);
				}
			}else{
				if((volumes[m].flag&(Fin|Fout))==(Fin|Fout) &&
				    liv==lov && riv==rov)
					j += snprint(buf+j, sizeof(buf)-j,
						" left %d right %d",
						liv, riv);
				else{
					if(volumes[m].flag & Fin)
						j += snprint(buf+j, sizeof(buf)-j,
							" in left %d right %d",
							liv, riv);
					if(volumes[m].flag & Fout)
						j += snprint(buf+j, sizeof(buf)-j,
							" out left %d right %d",
							lov, rov);
				}
			}
			j += snprint(buf+j, sizeof(buf)-j, "\n");
		}
		return readstr(offset, p, n, buf);
	}
	return n0-n;
}

static void
setrate(int newrate)
{
	switch (newrate) {
	case 16000:
	case 22050:
	case 44100:
	case 48000:
		volumes[Vspeed].ilval = volumes[Vspeed].irval = rate = newrate;
		break;
	default:
		error(Ebadarg);
	}
}

static void
setbufsize(int newbufsize)
{
	if (newbufsize < 0 || newbufsize > 8 * 1024 || newbufsize % sizeof(ulong))
		error(Ebadarg);
	bufsize = newbufsize;
}

static long
audiowrite(Chan *c, void *vp, long n, vlong)
{
	long m, n0;
	int i, nf, v, left, right, in, out;
	char buf[255], *field[Ncmd];
	char *p;
	IOstate *a;

	p = vp;
	n0 = n;
	switch((ulong)c->qid.path) {
	default:
		error(Eperm);
		break;

	case Qspeed:
		if(n > sizeof(buf)-1)
			n = sizeof(buf)-1;
		memmove(buf, p, n);
		buf[n] = '\0';
		nf = getfields(buf, field, Ncmd, 1, " \t\n");
		if (nf == 1)
			/* The user just supplied a number */
			setrate((int)strtol(field[0], (char **)nil, 0));
		else {
			i = 0;
			while (i < nf) {
				int newval;

				if (i + 1 >= nf) error(Ebadarg);
				newval = (int)strtol(field[i + 1], (char **)nil, 0);
				if (!strcmp(field[i], "speed"))
					setrate(newval);
				else if (!strcmp(field[i], "dmasize"))
					setbufsize(newval);
				else
					error(Ebadarg);
				i += 2;
			}
		}
		break;
		
	case Qvolume:
		v = Vaudio;
		left = 1;
		right = 1;
		in = 1;
		out = 1;
		if(n > sizeof(buf)-1)
			n = sizeof(buf)-1;
		memmove(buf, p, n);
		buf[n] = '\0';

		nf = getfields(buf, field, Ncmd, 1, " \t\n");
		for(i = 0; i < nf; i++){
			/*
			 * a number is volume
			 */
			if(field[i][0] >= '0' && field[i][0] <= '9') {
				m = strtoul(field[i], 0, 10);
				if (m < 0 || m > 100) 
					error(Evolume);
				if(left && out)
					audio.lovol[v] = m;
				if(left && in)
					audio.livol[v] = m;
				if(right && out)
					audio.rovol[v] = m;
				if(right && in)
					audio.rivol[v] = m;
				goto cont0;
			}

			for(m=0; volumes[m].name; m++) {
				if(strcmp(field[i], volumes[m].name) == 0) {
					v = m;
					in = 1;
					out = 1;
					left = 1;
					right = 1;
					goto cont0;
				}
			}

			if(strcmp(field[i], "reset") == 0) {
				resetlevel();
				goto cont0;
			}
			if(strcmp(field[i], "debug") == 0) {
				debug = debug?0:1;
				goto cont0;
			}
			if(strcmp(field[i], "in") == 0) {
				in = 1;
				out = 0;
				goto cont0;
			}
			if(strcmp(field[i], "out") == 0) {
				in = 0;
				out = 1;
				goto cont0;
			}
			if(strcmp(field[i], "left") == 0) {
				left = 1;
				right = 0;
				goto cont0;
			}
			if(strcmp(field[i], "right") == 0) {
				left = 0;
				right = 1;
				goto cont0;
			}
			if(strcmp(field[i], "reg") == 0) {
				if(nf < 3)
					error(Evolume);
				setreg(field[1], atoi(field[2]), nf == 4 ? atoi(field[3]):1);
				return n0;
			}
			error(Evolume);
			break;
		cont0:;
		}
		mxvolume();
		break;

	case Qaudio:
		if (debug > 1) print("#A: write %ld\n", n);
		if((audio.amode & Awrite) == 0)
			error(Emode);
		a = &audio.o;
		qlock(a);
		if(waserror()){
			qunlock(a);
			nexterror();
		}
		while(n > 0) {
			/* wait if dma in progress */
			while (!dmaidle(a->dma) && !zerodma && a->filling == a->current) {
				if (debug > 1) print("#A: sleep\n");
				sleep(&a->vous, audioqnotfull, a);
			}
			m = bufsize - a->filling->nbytes;
			if(m > n)
				m = n;
			memmove(a->filling->virt + a->filling->nbytes, p, m);

			a->filling->nbytes += m;
			n -= m;
			p += m;
			if(a->filling->nbytes >= bufsize) {
				if (debug > 1) print("#A: filled @%p\n", a->filling);
				a->filling++;
				if (a->filling == &a->buf[Nbuf])
					a->filling = a->buf;
				sendaudio(a);
			}
		}
		poperror();
		qunlock(a);
		break;
	}
	return n0 - n;
}

Dev uda1341devtab = {
	'A',
	"audio",

	devreset,
	audioinit,
	audioattach,
	audiowalk,
	audiostat,
	audioopen,
	devcreate,
	audioclose,
	audioread,
	devbread,
	audiowrite,
	devbwrite,
	devremove,
	devwstat,
	audiopower,
};
