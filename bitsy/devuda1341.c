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
typedef struct	IOstate IOstate;

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

	Bufsize		= 8*1024,	/* 46 ms each */
	Nbuf		= 32,		/* 1.5 seconds total */

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
	int		active;			/* dma running */
	uchar*	virt;
	uint	nbytes;
};

struct	IOstate
{
	QLock;
	Rendez	vous;
	int		bufinit;		/* boolean, if buffers allocated */
	Buf		buf[Nbuf];		/* buffers and queues */
	Buf		*current;		/* next candidate for dma */
	Buf		*filling;		/* buffer being filled */
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

static void
bufinit(IOstate *b)
{
	int i;

	for (i = 0; i < Nbuf; i++)
		b->buf[i].virt = xalloc(Bufsize);
	b->bufinit = 1;
};

static void
setempty(IOstate *b)
{
	int i;

	for (i = 0; i < Nbuf; i++) {
		b->buf[i].nbytes = 0;
		b->buf[i].active = 0;
	}
	b->filling = b->buf;
	b->current = b->buf;
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

static void
sendaudio(IOstat *b) {
	if (dmastart(b->chan, b->current->virt, b->current->nbytes)) {
		b->current->active++;
		b->current++;
		if (b->current == &b->buf[Nbuf])
			b->current == &b->buf[0];
	}
}

static Chan*
audioattach(char *param)
{
	return devattach('A', param);
}

static int
audiowalk(Chan *c, char *name)
{
	return devwalk(c, name, audiodir, nelem(audiodir), devgen);
}

static void
audiostat(Chan *c, char *db)
{
	devstat(c, db, audiodir, nelem(audiodir), devgen);
}

static Chan*
audioopen(Chan *c, int omode)
{

	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qstatus:
		if((omode&7) != OREAD)
			error(Eperm);
	case Qvolume:
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
		if (omode & Aread) {
			/* read */
			audio.amode |= Aread;
			if(audio.i.bufinit == 0)
				bufinit(&audio.i);
			setempty(&audio.i);
		}
		if (omode & 0x2) {
			/* write */
			audio.amode |= Awrite;
			if(audio.o.bufinit == 0)
				bufinit(&audio.o);
			setempty(&audio.o);
		}
		qunlock(&audio);
//		mxvolume();
		break;
	}
	c = devopen(c, omode, audiodir, nelem(audiodir), devgen);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;

	return c;
}

static void
audioclose(Chan *c)
{
	Buf *b;

	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qdir:
	case Qvolume:
	case Qstatus:
		break;

	case Qaudio:
		if(c->flag & COPEN) {
			qlock(&audio);
			if(audio.amode & Awrite) {
				/* flush out last partial buffer */
				b = audio.o.filling;
				if(audio.o.buf[b].count) {
					putbuf(&audio.full, b);
				}
			}
			audio.amode = Aclosed;
			if(waserror()){
				qunlock(&audio);
				nexterror();
			}
			while(audio.active)
				waitaudio();
			setempty();
			poperror();
			qunlock(&audio);
		}
		break;
	}
}

static long
audioread(Chan *c, void *v, long n, vlong off)
{
	int liv, riv, lov, rov;
	long m, n0;
	char buf[300];
	Buf *b;
	int j;
	ulong offset = off;
	char *a;

	n0 = n;
	a = v;
	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qdir:
		return devdirread(c, a, n, audiodir, nelem(audiodir), devgen);

	case Qaudio:
		if(audio.amode != Aread)
			error(Emode);
		qlock(&audio);
		if(waserror()){
			qunlock(&audio);
			nexterror();
		}
		while(n > 0) {
			b = audio.filling;
			if(b == 0) {
				b = getbuf(&audio.full);
				if(b == 0) {
					waitaudio();
					continue;
				}
				audio.filling = b;
				swab(b->virt);
				audio.curcount = 0;
			}
			m = Bufsize-audio.curcount;
			if(m > n)
				m = n;
			memmove(a, b->virt+audio.curcount, m);

			audio.curcount += m;
			n -= m;
			a += m;
			if(audio.curcount >= Bufsize) {
				audio.filling = 0;
				putbuf(&audio.empty, b);
			}
		}
		poperror();
		qunlock(&audio);
		break;

	case Qstatus:
		buf[0] = 0;
		snprint(buf, sizeof(buf), "bytes %lud\ntime %lld\n",
			audio.totcount, audio.tottime);
		return readstr(offset, a, n, buf);

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
		return readstr(offset, a, n, buf);
	}
	return n0-n;
}

static long
audiowrite(Chan *c, void *vp, long n, vlong)
{
	long m, n0;
	int i, nf, v, left, right, in, out;
	char buf[255], *field[Ncmd];
	Buf *b;
	char *p;
	IOstate *a;

	p = vp;
	n0 = n;
	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
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
				if(left && out)
					audio.lovol[v] = m;
				if(left && in)
					audio.livol[v] = m;
				if(right && out)
					audio.rovol[v] = m;
				if(right && in)
					audio.rivol[v] = m;
				mxvolume();
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
				mxvolume();
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
			error(Evolume);
			break;
		cont0:;
		}
		break;

	case Qaudio:
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
			while (a->filling->active)
				waitaudio(a);

			m = Bufsize - a->filling->nbytes;
			if(m > n)
				m = n;
			memmove(a->filling->buf + a->filling->nbytes, p, m);

			a->filling->nbytes += m;
			n -= m;
			p += m;
			if(a->filling->nbytes >= Bufsize) {
				sendaudio(a);
				a->filling++;
				if (a->filling == &a->buf[Nbuf])
					a->filling = a->buf;
			}
		}
		poperror();
		qunlock(a);
		break;
	}
	return n0 - n;
}

Dev audiodevtab = {
	'A',
	"audio",

	devreset,
	audioinit,
	audioattach,
	devclone,
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
};
