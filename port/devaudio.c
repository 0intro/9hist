/*
 *	SB 16 driver
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"devtab.h"
#include	"io.h"
#include	"audio.h"

#define	NPORT		(sizeof audiodir/sizeof(Dirtab))

typedef struct	Buf	Buf;
typedef struct	Level	Level;

enum
{
	Qdir		= 0,
	Qaudio,
	Qvolume,

	Fmono		= 1,

	Aclosed		= 0,
	Aread,
	Awrite,

	Speed		= 44100,
	Ncmd		= 50,		/* max volume command words */
};

Dirtab
audiodir[] =
{
	"audio",	{Qaudio},		0,	0666,
	"volume",	{Qvolume},		0,	0666,
};

struct	Level
{
	int	master;		/* mixer output volume control */
	int	ogain;		/* extra gain after master */
	int	pcm;		/* mixer volume on D/A (voice) */
	int	synth;		/* mixer volume on synthesizer (MIDI) */
	int	cd;		/* mixer volume on cd */
	int	line;		/* mixer volume on line */
	int	igain;		/* mixer volume to A/D */
	int	treb;		/* treb control */
	int	bass;		/* bass control */
	int	iswitch;	/* input on/off switches */
};
struct	Buf
{
	uchar*	virt;
	ulong	phys;
	Buf*	next;
};
struct	Queue
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
	Level	left;		/* all of left volumes */
	Level	right;		/* all of right volumes */
	int	mic;		/* mono level */
	int	speaker;	/* mono level */
	int	oswitch;	/* output on/off switches */
	int	speed;		/* pcm sample rate, doesnt change w stereo */
	int	major;		/* SB16 major version number (sb 4) */
	int	minor;		/* SB16 minor version number */

	Buf	buf[Nbuf];	/* buffers and queues */
	Queue	empty;
	Queue	full;
	Buf*	current;
	Buf*	filling;
} audio;

static	struct
{
	char*	name;
	int*	ptr1;
	int*	ptr2;
	int	flag;
	int	ilval;
	int	irval;
} volumes[] =
{
	"master",	&audio.left.master,	&audio.right.master,	0,	50,	50,
	"ogain",	&audio.left.ogain,	&audio.right.ogain,	0, 	0,	0,
	"igain",	&audio.left.igain,	&audio.right.igain,	0, 	0,	0,

	"treb",		&audio.left.treb,	&audio.right.treb,	0, 	50,	50,
	"bass",		&audio.left.bass,	&audio.right.bass,	0, 	50,	50,

	"pcm",		&audio.left.pcm,	&audio.right.pcm,	0, 	90,	90,
	"synth",	&audio.left.synth,	&audio.right.synth,	0,	90,	90,
	"cd",		&audio.left.cd,		&audio.right.cd,	0,	81,	81,
	"line",		&audio.left.line,	&audio.right.line,	0,	65,	65,

	"mic",		&audio.mic,		&audio.mic,		Fmono,	0,	0,
	"speaker",	&audio.speaker,		&audio.speaker,		Fmono,	0,	0,
	"oswitch",	&audio.oswitch,		&audio.oswitch,		Fmono,	31,	31,
	"iswitch",	&audio.left.iswitch,	&audio.right.iswitch,	0,	85,	43,

	"speed",	&audio.speed,		&audio.speed,		Fmono,	Speed,	Speed,
	0
};

static struct
{
	Lock;
	int	reset;		/* io ports to the sound blaster */
	int	read;
	int	write;
	int	wstatus;
	int	rstatus;
	int	mixaddr;
	int	mixdata;
	int	clri8;
	int	clri16;
	int	clri401;
} blaster;

static	void	swab(uchar*);

static	char	Emajor[]	= "SoundBlaster version too old";
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
/*	print("SB16 sbcmd (#%.2x) timeout\n", val);	/**/
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
/*	print("SB16 sbread did not respond\n");	/**/
	return 0xbb;
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
	ilock(&blaster);
	mxcmds(0x30, audio.left.master);
	mxcmds(0x31, audio.right.master);

	mxcmdt(0x32, audio.left.pcm);
	mxcmdt(0x33, audio.right.pcm);

	mxcmdt(0x34, audio.left.synth);
	mxcmdt(0x35, audio.right.synth);

	mxcmdt(0x36, audio.left.cd);
	mxcmdt(0x37, audio.right.cd);

	mxcmdt(0x38, audio.left.line);
	mxcmdt(0x39, audio.right.line);

	mxcmdt(0x3a, audio.mic);
	mxcmdt(0x3b, audio.speaker);

	mxcmds(0x3f, audio.left.igain);
	mxcmds(0x40, audio.right.igain);
	mxcmds(0x41, audio.left.ogain);
	mxcmds(0x42, audio.right.ogain);

	mxcmdu(0x44, audio.left.treb);
	mxcmdu(0x45, audio.right.treb);

	mxcmdu(0x46, audio.left.bass);
	mxcmdu(0x47, audio.right.bass);

	mxcmd(0x3c, audio.oswitch);
	mxcmd(0x3d, audio.left.iswitch);
	mxcmd(0x3e, audio.right.iswitch);
	iunlock(&blaster);
}

static	Buf*
getbuf(Queue *q)
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
putbuf(Queue *q, Buf *b)
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
		if(b)	/* shouldnt happen */
			putbuf(&audio.full, b);
		b = getbuf(&audio.empty);
	} else {
		if(b)	/* shouldnt happen */
			putbuf(&audio.empty, b);
		b = getbuf(&audio.full);
	}
	audio.current = b;
	if(b == 0)
		goto shutdown;

	dmasetup(Dma, b->virt, Bufsize, audio.amode == Aread);
	return;

shutdown:
	dmaend(Dma);
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
startdma(void)
{
	ulong count;

	ilock(&blaster);
	dmaend(Dma);
	if(audio.amode == Aread)
		sbcmd(0x42);			/* input sampling rate */
	else
		sbcmd(0x41);			/* output sampling rate */
	sbcmd(audio.speed>>8);
	sbcmd(audio.speed);

	count = (Bufsize >> 1) - 1;
	if(audio.amode == Aread)
		sbcmd(0xbe);			/* A/D, autoinit */
	else
		sbcmd(0xb6);			/* D/A, autoinit */
	sbcmd(0x30);				/* stereo, 16 bit */
	sbcmd(count);
	sbcmd(count>>8);

	audio.active = 1;
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
		startdma();
}

void
audiosbintr(void)
{
	int stat, dummy;

	stat = mxread(0x82) & 7;		/* get irq status */
	if(stat) {
		dummy = 0;
		if(stat & 2) {
			ilock(&blaster);
			dummy = inb(blaster.clri16);
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

void
pcaudiosbintr(Ureg *ureg, void *rock)
{
	USED(ureg, rock);
/*	print("sb16 audio interrupt\n");	/**/
	audiosbintr();
}

void
audiodmaintr(void)
{
/*	print("sb16 dma interrupt\n");	/**/
}

static int
anybuf(void *p)
{
	USED(p);
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
		print("audio timeout\n");	/**/
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
	iunlock(&blaster);
}

void
audioreset(void)
{
}

static	void
resetlevel(void)
{
	int i;

	for(i=0; volumes[i].name; i++) {
		*volumes[i].ptr1 = volumes[i].ilval;
		*volumes[i].ptr2 = volumes[i].irval;
	}
}

void
audioinit(void)
{
	ISAConf sbconf;
	int i;

	sbconf.port = 0x220;
	sbconf.irq = 7;
	if(isaconfig("audio", 0, &sbconf) == 0)
		return;
	if(strcmp(sbconf.type, "sb16") != 0)
		return;
	switch(sbconf.port){
	case 0x220:
	case 0x240:
	case 0x260:
	case 0x280:
		break;
	default:
		print("bad sb16 port 0x%x\n", sbconf.port);
		return;
	}
	switch(sbconf.irq){
	case 2:
	case 5:
	case 7:
	case 10:
		break;
	default:
		print("bad sb16 irq %d\n", sbconf.irq);
		return;
	}

	blaster.reset = sbconf.port + 0x6;
	blaster.read = sbconf.port + 0xa;
	blaster.write = sbconf.port + 0xc;
	blaster.wstatus = sbconf.port + 0xc;
	blaster.rstatus = sbconf.port + 0xe;
	blaster.mixaddr = sbconf.port + 0x4;
	blaster.mixdata = sbconf.port + 0x5;
	blaster.clri8 = sbconf.port + 0xe;
	blaster.clri16 = sbconf.port + 0xf;
	blaster.clri401 = sbconf.port + 0x100;

	seteisadma(Dma, audiodmaintr);
	setvec(Int0vec+sbconf.irq, pcaudiosbintr, 0);

	audio.amode = Aclosed;
	resetlevel();

	outb(blaster.reset, 1);
	delay(1);			/* >3 υs */
	outb(blaster.reset, 0);
	delay(1);

	i = sbread();
	if(i != 0xaa) {
		print("sound blaster didnt respond #%.2x\n", i);
		return;
	}

	sbcmd(0xe1);			/* get version */
	audio.major = sbread();
	audio.minor = sbread();

	if(audio.major != 4) {
		print("bad soundblaster model #%.2x #.2x\n", audio.major, audio.minor);
		return;
	}
	/*
	 * initialize the mixer
	 */
	mxcmd(0x00, 0);			/* Reset mixer */
	mxvolume();

	/*
	 * set up irq/dma chans
	 */
	mxcmd(0x80,			/* irq */
		(sbconf.irq==2)? 1:
		(sbconf.irq==5)? 2:
		(sbconf.irq==7)? 4:
		(sbconf.irq==10)? 8:
		0);
	mxcmd(0x81, 1<<Dma);	/* dma */
}

Chan*
audioattach(char *param)
{
	return devattach('A', param);
}

Chan*
audioclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
audiowalk(Chan *c, char *name)
{
	return devwalk(c, name, audiodir, NPORT, devgen);
}

void
audiostat(Chan *c, char *db)
{
	devstat(c, db, audiodir, NPORT, devgen);
}

Chan*
audioopen(Chan *c, int omode)
{
	int amode;

	if(audio.major != 4)
		error(Emajor);

	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qvolume:
	case Qdir:
		break;

	case Qaudio:
		amode = Awrite;
		if((omode&7) == OREAD)
			amode = Aread;
		qlock(&audio);
		if(audio.amode != Aclosed){
			qunlock(&audio);
			error(Einuse);
		}
		if(audio.bufinit == 0) {
			audio.bufinit = 1;
			sbbufinit();
		}
		audio.amode = amode;
		setempty();
		audio.curcount = 0;
		qunlock(&audio);
		break;
	}
	c = devopen(c, omode, audiodir, NPORT, devgen);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;

	return c;
}

void
audiocreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c);
	USED(name);
	USED(omode);
	USED(perm);

	error(Eperm);
}

void
audioclose(Chan *c)
{

	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qdir:
	case Qvolume:
		break;

	case Qaudio:
		if(c->flag & COPEN) {
			qlock(&audio);
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

long
audioread(Chan *c, char *a, long n, ulong offset)
{
	long m, o, n0, bn;
	char buf[256];
	Buf *b;
	int j;

	n0 = n;
	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qdir:
		return devdirread(c, a, n, audiodir, NPORT, devgen);

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

	case Qvolume:
		j = 0;
		for(m=0; volumes[m].name; m++) {
			o = *volumes[m].ptr1;
			if(volumes[m].flag & Fmono)
				j += snprint(buf+j, sizeof(buf)-j, "%s %d\n", volumes[m].name, o);
			else {
				bn = *volumes[m].ptr2;
				if(o == bn)
					j += snprint(buf+j, sizeof(buf)-j, "%s both %d\n", volumes[m].name, o);
				else
					j += snprint(buf+j, sizeof(buf)-j, "%s left %d right %d\n",
						volumes[m].name, o, bn);
			}
		}

		return readstr(offset, a, n, buf);
	}
	return n0-n;
}

Block*
audiobread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

long
audiowrite(Chan *c, char *a, long n, ulong offset)
{
	long m, n0;
	int i, nf, v, left, right;
	char buf[255], *field[Ncmd];
	Buf *b;

	USED(offset);

	n0 = n;
	switch(c->qid.path & ~CHDIR) {
	default:
		error(Eperm);
		break;

	case Qvolume:
		v = 0;
		left = 1;
		right = 1;
		if(n > sizeof(buf)-1)
			n = sizeof(buf)-1;
		memmove(buf, a, n);
		buf[n] = '\0';

		nf = getfields(buf, field, Ncmd, " \t\n");
		for(i = 0; i < nf; i++){
			/*
			 * a number is volume
			 */
			if(field[i][0] >= '0' && field[i][0] <= '9') {
				m = strtoul(field[i], 0, 10);
				if(left)
					*volumes[v].ptr1 = m;
				if(right)
					*volumes[v].ptr2 = m;
				mxvolume();
				goto cont0;
			}

			for(m=0; volumes[m].name; m++) {
				if(strcmp(field[i], volumes[m].name) == 0) {
					v = m;
					goto cont0;
				}
			}

			if(strcmp(field[i], "reset") == 0) {
				resetlevel();
				mxvolume();
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
			if(strcmp(field[i], "both") == 0) {
				left = 1;
				right = 1;
				goto cont0;
			}
			error(Evolume);
			break;
		cont0:;
		}
		break;

	case Qaudio:
		if(audio.amode != Awrite)
			error(Emode);
		qlock(&audio);
		if(waserror()){
			qunlock(&audio);
			nexterror();
		}
		while(n > 0) {
			b = audio.filling;
			if(b == 0) {
				b = getbuf(&audio.empty);
				if(b == 0) {
					waitaudio();
					continue;
				}
				audio.filling = b;
				audio.curcount = 0;
			}

			m = Bufsize-audio.curcount;
			if(m > n)
				m = n;
			memmove(b->virt+audio.curcount, a, m);

			audio.curcount += m;
			n -= m;
			a += m;
			if(audio.curcount >= Bufsize) {
				audio.filling = 0;
				swab(b->virt);
				putbuf(&audio.full, b);
			}
		}
		poperror();
		qunlock(&audio);
		break;
	}
	return n0 - n;
}

long
audiobwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

void
audioremove(Chan *c)
{
	USED(c);

	error(Eperm);
}

void
audiowstat(Chan *c, char *dp)
{
	USED(c);
	USED(dp);

	error(Eperm);
}

static	void
swab(uchar *a)
{
	ulong *p, *ep, b;

	if(!SBswab)
		return;
	p = (ulong*)a;
	ep = p + (Bufsize>>2);
	while(p < ep) {
		b = *p;
		b = (b>>24) | (b<<24) |
			((b&0xff0000) >> 8) |
			((b&0x00ff00) << 8);
		*p++ = b;
	}
}
