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
static inline void L3_acquirepins(void)
{
	GPSR = (GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
	GPDR |=  (GPIO_L3_SCLK_o | GPIO_L3_SDA_io);
}

/*
 * Release control of the IIC/L3 shared pins
 */
static inline void L3_releasepins(void)
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
static inline int L3_getbit(void)
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
static inline void L3_sendbit(int bit)
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

static	void	swab(uchar*);

static	char	Emajor[]	= "soundblaster not responding/wrong version";
static	char	Emode[]		= "illegal open mode";
static	char	Evolume[]	= "illegal volume specifier";

static	int
sbcmd(int val)
{
	int i, s;

	for(i=1<<16; i!=0; i--) {
		s = inb(blaster.wstatus);
		if((s & 0x80) == 0) {
			outb(blaster.write, val);
			return 0;
		}
	}
/*	print("#A: sbcmd (0x%.2x) timeout\n", val);	/**/
	return 1;
}

static	int
sbread(void)
{
	int i, s;

	for(i=1<<16; i!=0; i--) {
		s = inb(blaster.rstatus);
		if((s & 0x80) != 0) {
			return inb(blaster.read);
		}
	}
/*	print("#A: sbread did not respond\n");	/**/
	return -1;
}

static int
ess1688w(int reg, int val)
{
	if(sbcmd(reg) || sbcmd(val))
		return 1;

	return 0;
}

static int
ess1688r(int reg)
{
	if(sbcmd(0xC0) || sbcmd(reg))
		return -1;

	return sbread();
}

static	int
mxcmd(int addr, int val)
{

	outb(blaster.mixaddr, addr);
	outb(blaster.mixdata, val);
	return 1;
}

static	int
mxread(int addr)
{
	int s;

	outb(blaster.mixaddr, addr);
	s = inb(blaster.mixdata);
	return s;
}

static	void
mxcmds(int s, int v)
{

	if(v > 100)
		v = 100;
	if(v < 0)
		v = 0;
	mxcmd(s, (v*255)/100);
}

static	void
mxcmdt(int s, int v)
{

	if(v > 100)
		v = 100;
	if(v <= 0)
		mxcmd(s, 0);
	else
		mxcmd(s, 255-100+v);
}

static	void
mxcmdu(int s, int v)
{

	if(v > 100)
		v = 100;
	if(v <= 0)
		v = 0;
	mxcmd(s, 128-50+v);
}

static	void
mxvolume(void)
{
	int *left, *right;
	int source;

	if(audio.amode == Aread){
		left = audio.livol;
		right = audio.rivol;
	}else{
		left = audio.lovol;
		right = audio.rovol;
	}

	ilock(&blaster);

	mxcmd(0x30, 255);		/* left master */
	mxcmd(0x31, 255);		/* right master */
	mxcmd(0x3f, 0);		/* left igain */
	mxcmd(0x40, 0);		/* right igain */
	mxcmd(0x41, 0);		/* left ogain */
	mxcmd(0x42, 0);		/* right ogain */

	mxcmds(0x32, left[Vaudio]);
	mxcmds(0x33, right[Vaudio]);

	mxcmds(0x34, left[Vsynth]);
	mxcmds(0x35, right[Vsynth]);

	mxcmds(0x36, left[Vcd]);
	mxcmds(0x37, right[Vcd]);

	mxcmds(0x38, left[Vline]);
	mxcmds(0x39, right[Vline]);

	mxcmds(0x3a, left[Vmic]);
	mxcmds(0x3b, left[Vspeaker]);

	mxcmdu(0x44, left[Vtreb]);
	mxcmdu(0x45, right[Vtreb]);

	mxcmdu(0x46, left[Vbass]);
	mxcmdu(0x47, right[Vbass]);

	source = 0;
	if(left[Vsynth])
		source |= 1<<6;
	if(right[Vsynth])
		source |= 1<<5;
	if(left[Vaudio])
		source |= 1<<4;
	if(right[Vaudio])
		source |= 1<<3;
	if(left[Vcd])
		source |= 1<<2;
	if(right[Vcd])
		source |= 1<<1;
	if(left[Vmic])
		source |= 1<<0;
	if(audio.amode == Aread)
		mxcmd(0x3c, 0);		/* output switch */
	else
		mxcmd(0x3c, source);
	mxcmd(0x3d, source);		/* input left switch */
	mxcmd(0x3e, source);		/* input right switch */
	iunlock(&blaster);
}

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

/*
 * move the dma to the next buffer
 */
static	void
contindma(void)
{
	Buf *b;

	if(!audio.active)
		goto shutdown;

	b = audio.current;
	if(audio.amode == Aread) {
		if(b)	/* shouldn't happen */
			putbuf(&audio.full, b);
		b = getbuf(&audio.empty);
	} else {
		if(b)	/* shouldn't happen */
			putbuf(&audio.empty, b);
		b = getbuf(&audio.full);
	}
	audio.current = b;
	if(b == 0)
		goto shutdown;

	if(dmasetup(blaster.dma, b->virt, Bufsize, audio.amode == Aread) >= 0)
		return;
	print("#A: dmasetup fail\n");
	putbuf(&audio.empty, b);

shutdown:
	dmaend(blaster.dma);
	sbcmd(0xd9);				/* exit at end of count */
	sbcmd(0xd5);				/* pause */
	audio.curcount = 0;
	audio.active = 0;
}

/*
 * cause sb to get an interrupt per buffer.
 * start first dma
 */
static	void
sb16startdma(void)
{
	ulong count;
	int speed;

	ilock(&blaster);
	dmaend(blaster.dma);
	if(audio.amode == Aread) {
		sbcmd(0x42);			/* input sampling rate */
		speed = audio.livol[Vspeed];
	} else {
		sbcmd(0x41);			/* output sampling rate */
		speed = audio.lovol[Vspeed];
	}
	sbcmd(speed>>8);
	sbcmd(speed);

	count = (Bufsize >> 1) - 1;
	if(audio.amode == Aread)
		sbcmd(0xbe);			/* A/D, autoinit */
	else
		sbcmd(0xb6);			/* D/A, autoinit */
	sbcmd(0x30);				/* stereo, 16 bit */
	sbcmd(count);
	sbcmd(count>>8);

	audio.active = 1;
	audio.tottime = todget(nil);
	contindma();
	iunlock(&blaster);
}

static int
ess1688reset(void)
{
	int i;

	outb(blaster.reset, 3);
	delay(1);			/* >3 υs */
	outb(blaster.reset, 0);
	delay(1);

	i = sbread();
	if(i != 0xAA) {
		print("#A: no response 0x%.2x\n", i);
		return 1;
	}

	if(sbcmd(0xC6)){		/* extended mode */
		print("#A: barf 3\n");
		return 1;
	}

	return 0;
}

static	void
ess1688startdma(void)
{
	ulong count;
	int speed, x;

	ilock(&blaster);
	dmaend(blaster.dma);

	if(audio.amode == Awrite)
		ess1688reset();
	if(audio.amode == Aread)
		sbcmd(0xD3);			/* speaker off */

	/*
	 * Set the speed.
	 */
	if(audio.amode == Aread)
		speed = audio.livol[Vspeed];
	else
		speed = audio.lovol[Vspeed];
	if(speed < 4000)
		speed = 4000;
	else if(speed > 48000)
		speed = 48000;

	if(speed > 22000)
		  x = 0x80|(256-(795500+speed/2)/speed);
	else
		  x = 128-(397700+speed/2)/speed;
	ess1688w(0xA1, x & 0xFF);

	speed = (speed * 9) / 20;
	x = 256 - 7160000 / (speed * 82);
	ess1688w(0xA2, x & 0xFF);

	if(audio.amode == Aread)
		ess1688w(0xB8, 0x0E);		/* A/D, autoinit */
	else
		ess1688w(0xB8, 0x04);		/* D/A, autoinit */
	x = ess1688r(0xA8) & ~0x03;
	ess1688w(0xA8, x|0x01);			/* 2 channels */
	ess1688w(0xB9, 2);			/* demand mode, 4 bytes per request */

	if(audio.amode == Awrite)
		ess1688w(0xB6, 0);
	ess1688w(0xB7, 0x71);
	ess1688w(0xB7, 0xBC);

	x = ess1688r(0xB1) & 0x0F;
	ess1688w(0xB1, x|0x50);
	x = ess1688r(0xB2) & 0x0F;
	ess1688w(0xB2, x|0x50);
	if(audio.amode == Awrite)
		sbcmd(0xD1);			/* speaker on */

	count = -Bufsize;
	ess1688w(0xA4, count & 0xFF);
	ess1688w(0xA5, (count>>8) & 0xFF);
	x = ess1688r(0xB8);
	ess1688w(0xB8, x|0x05);

	audio.active = 1;
	audio.tottime = todget(nil);
	contindma();
	iunlock(&blaster);
}

/*
 * if audio is stopped,
 * start it up again.
 */
static	void
pokeaudio(void)
{
	if(!audio.active)
		blaster.startdma();
}

static void
sb16intr(void)
{
	int stat, dummy;

	stat = mxread(0x82) & 7;		/* get irq status */
	if(stat) {
		dummy = 0;
		if(stat & 2) {
			ilock(&blaster);
			dummy = inb(blaster.clri16);
			audio.totcount += Bufsize;
			audio.tottime = todget(nil);
			contindma();
			iunlock(&blaster);
			audio.intr = 1;
			wakeup(&audio.vous);
		}
		if(stat & 1) {
			dummy = inb(blaster.clri8);
		}
		if(stat & 4) {
			dummy = inb(blaster.clri401);
		}
		USED(dummy);
	}
}

static void
ess1688intr(void)
{
	int dummy;

	if(audio.active){
		ilock(&blaster);
		audio.totcount += Bufsize;
		audio.tottime = todget(nil);
		contindma();
		dummy = inb(blaster.clri8);
		iunlock(&blaster);
		audio.intr = 1;
		wakeup(&audio.vous);
		USED(dummy);
	}
	else
		print("#A: unexpected ess1688 interrupt\n");
}

void
audiosbintr(void)
{
	/*
	 * Carrera interrupt interface.
	 */
	blaster.intr();
}

static void
pcaudiosbintr(Ureg*, void*)
{
	/*
	 * x86 interrupt interface.
	 */
	blaster.intr();
}

void
audiodmaintr(void)
{
/*	print("#A: dma interrupt\n");	/**/
}

static int
anybuf(void*)
{
	return audio.intr;
}

/*
 * wait for some output to get
 * empty buffers back.
 */
static void
waitaudio(void)
{

	audio.intr = 0;
	pokeaudio();
	tsleep(&audio.vous, anybuf, 0, 10*1000);
	if(audio.intr == 0) {
/*		print("#A: audio timeout\n");	/**/
		audio.active = 0;
		pokeaudio();
	}
}

static void
sbbufinit(void)
{
	int i;
	void *p;

	for(i=0; i<Nbuf; i++) {
		p = xspanalloc(Bufsize, CACHELINESZ, 64*1024);
		dcflush(p, Bufsize);
		audio.buf[i].virt = UNCACHED(uchar, p);
		audio.buf[i].phys = (ulong)PADDR(p);
	}
}

static	void
setempty(void)
{
	int i;

	ilock(&blaster);
	audio.empty.first = 0;
	audio.empty.last = 0;
	audio.full.first = 0;
	audio.full.last = 0;
	audio.current = 0;
	audio.filling = 0;
	for(i=0; i<Nbuf; i++)
		putbuf(&audio.empty, &audio.buf[i]);
	audio.totcount = 0;
	audio.tottime = 0LL;
	iunlock(&blaster);
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

static int
ess1688(ISAConf* sbconf)
{
	int i, major, minor;

	/*
	 * Try for ESS1688.
	 */
	sbcmd(0xE7);			/* get version */
	major = sbread();
	minor = sbread();
	if(major != 0x68 || minor != 0x8B){
		print("#A: model 0x%.2x 0x%.2x; not ESS1688 compatible\n", major, minor);
		return 1;
	}

	ess1688reset();

	switch(sbconf->irq){
	case 2:
	case 9:
		i = 0x50|(0<<2);
		break;
	case 5:
		i = 0x50|(1<<2);
		break;
	case 7:
		i = 0x50|(2<<2);
		break;
	case 10:
		i = 0x50|(3<<2);
		break;
	default:
		print("#A: bad ESS1688 irq %lud\n", sbconf->irq);
		return 1;
	}
	ess1688w(0xB1, i);

	switch(sbconf->dma){
	case 0:
		i = 0x50|(1<<2);
		break;
	case 1:
		i = 0xF0|(2<<2);
		break;
	case 3:
		i = 0x50|(3<<2);
		break;
	default:
		print("#A: bad ESS1688 dma %lud\n", sbconf->dma);
		return 1;
	}
	ess1688w(0xB2, i);

	ess1688reset();

	blaster.startdma = ess1688startdma;
	blaster.intr = ess1688intr;

	return 0;
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
