#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"io.h"

#include	"devlml.h"

static void *		pciPhysBaseAddr;
static ulong		pciBaseAddr;
static Pcidev *		pcidev;

#define DBGREAD 0x01
#define DBGWRIT 0x02
#define DBGINTR	0x04
#define DBGINTS	0x08

int debug = 0;

// Lml 22 driver

enum{
	Qdir,
	Qjvideo,
	Qjframe,
	Qjcount,
};

static Dirtab lmldir[]={
//	 name,		 qid,		size,		mode
	"jvideo",	{Qjvideo},	0,		0666,
	"jframe",	{Qjframe},	0,		0666,
	"jcount",	{Qjcount},	0,		0444,
};

CodeData *	codeData;

typedef enum {
	New,
	Header,
	Body,
	Error,
} State;

int		frameNo;
Rendez		sleeper;
int		singleFrame;
int		nopens;
uchar		q856[3];
State		state = New;

static FrameHeader frameHeader = {
	MRK_SOI, MRK_APP3, (sizeof(FrameHeader)-4) << 8,
	{ 'L', 'M', 'L', '\0'},
	-1, 0, 0, 0, 0
};


#define writel(v, a) *(ulong *)(a) = (v)
#define readl(a) *(ulong*)(a)

static int
getbuffer(void){
	static last = NBUF-1;
	int l = last;

	for (;;) {
		last = (last+1) % NBUF;
		if (codeData->statCom[last] & STAT_BIT)
			return last;
		if (last == l)
			sleep(&sleeper, return0, 0);
	}
	return 0;
}

static long
vcount(Chan *, void *va, long nbytes, vlong) {
	char *p = (char *)va;

	// reads always return one byte: the next available buffer number
	if (nbytes <= 0) return 0;
	*p = getbuffer();
	return 1;
}

static long
vread(Chan *, void *va, long nbytes, vlong) {
	static int bufpos;
	static char *bufptr;
	static int curbuf;
	static int fragsize;
	static int frameno;
	static int frameprev;
	char *p;
	long count = nbytes;
	int i;
	vlong thetime;

	p = (char *)va;
	while (count > 0) {
		switch (state) {
		case New:
			curbuf = getbuffer();
			frameNo = codeData->statCom[curbuf] >> 24;
			fragsize = (codeData->statCom[curbuf] & 0x00ffffff)>>1;
			if (debug & DBGREAD)
				pprint("devlml: got read buf %d, fr %d, size %d\n",
					curbuf, frameNo, fragsize);
			if (!singleFrame && frameno != (frameprev + 1) % 256)
				pprint("Frame out of sequence: %d %d\n",
					frameprev, frameno);
			frameprev = frameno;
			if (fragsize <= 0 || fragsize > sizeof(Fragment)) {
				pprint("Wrong sized fragment, %d (ignored)\n",
					fragsize);
				codeData->statCom[curbuf] = PADDR(&(codeData->fragdesc[curbuf]));
				break;
			}
			// Fill in APP marker fields here
			thetime = todget();
			frameHeader.sec = (ulong)(thetime / 1000000000LL);
			frameHeader.usec = (ulong)(thetime % 1000000000LL) / 1000;
			frameHeader.frameSize = fragsize - 2 + sizeof(FrameHeader);
			frameHeader.frameSeqNo++;
			frameHeader.frameNo = frameno;
			bufpos = 0;
			state = Header;
			bufptr = (char *)(&frameHeader);
			// Fall through
		case Header:
			i = sizeof(FrameHeader) - bufpos;
			if (count <= i) {
				memmove(p, bufptr, count);
				bufptr += count;
				bufpos += count;
				return nbytes;
			}
			memmove(p, bufptr, i);
			count -= i;
			p += i;
			bufpos = 2;
			bufptr = codeData->frag[curbuf].fb + 2;
			state = Body;
			// Fall through
		case Body:
			i = fragsize - bufpos;
			if (count < i) {
				memmove(p, bufptr, count);
				bufptr += count;
				bufpos += count;
				return nbytes;
			}
			memmove(p, bufptr, i);
			count -= i;
			p += i;

			// Allow reuse of current buffer
			codeData->statCom[curbuf] = PADDR(&(codeData->fragdesc[curbuf]));
			state = New;
			if (singleFrame) {
				state = Error;
				return nbytes - count;
			}
			break;
		case Error:
			return 0;
		}
	}
}

static long
vwrite(Chan *, void *va, long nbytes, vlong) {
	static int bufpos;
	static char *bufptr;
	static int curbuf;
	static int fragsize;
	char *p;
	long count = nbytes;
	int i;

	p = (char *)va;
	while (count > 0) {
		switch (state) {
		case New:
			curbuf = getbuffer();
			if (debug&DBGWRIT)
				pprint("current buffer %d\n", curbuf);
			bufptr = codeData->frag[curbuf].fb;
			bufpos = 0;
			state = Header;
			// Fall through
		case Header:
			if (count < sizeof(FrameHeader) - bufpos) {
				memmove(bufptr, p, count);
				bufptr += count;
				bufpos += count;
				return nbytes;
			}
			// Fill remainder of header
			i = sizeof(FrameHeader) - bufpos;
			memmove(bufptr, p, i);
			bufptr += i;
			bufpos += i;
			p += i;
			count -= i;
			// verify header
			if (codeData->frag[curbuf].fh.mrkSOI != MRK_SOI
			 || codeData->frag[curbuf].fh.mrkAPP3 != MRK_APP3
			 || strcmp(codeData->frag[curbuf].fh.nm, APP_NAME)) {
				// Header is bad
				pprint("devlml: header error: 0x%.4ux, 0x%.4ux, `%.4s'\n",
					codeData->frag[curbuf].fh.mrkSOI,
					codeData->frag[curbuf].fh.mrkAPP3,
					codeData->frag[curbuf].fh.nm);
				state = Error;
				return nbytes - count;
			}
			fragsize = codeData->frag[curbuf].fh.frameSize;
			if (fragsize <= sizeof(FrameHeader)
			 || fragsize  > sizeof(Fragment)) {
				pprint("devlml: frame size error: 0x%.8ux\n",
					fragsize);
				state = Error;
				return nbytes - count;
			}
			state = Body;
			// Fall through
		case Body:
			i = fragsize - bufpos;
			if (count < i) {
				memmove(bufptr, p, count);
				bufptr += count;
				bufpos += count;
				return nbytes;
			}
			memmove(bufptr, p, i);
			bufptr += i;
			bufpos += i;
			p += i;
			count -= i;
			// We have written the frame, time to display it
			codeData->statCom[curbuf] = PADDR(&(codeData->fragdesc[curbuf]));
			if (debug&DBGWRIT)
				pprint("Sending buffer %d\n", curbuf);
			if (singleFrame) {
				state = Error;
				return nbytes - count;
			}
			state = New;
			break;
		case Error:
			return 0;
		}
	}
}

static void lmlintr(Ureg *, void *);

static void
lmlreset(void)
{
	Physseg segbuf;
	Physseg segreg;
	ulong regpa;
	ulong cdsize;
	int i;

	pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (pcidev == nil) {
		return;
	}
	cdsize = (sizeof(CodeData) + BY2PG - 1) & ~(BY2PG - 1);
	codeData = (CodeData*)xspanalloc(cdsize, BY2PG, 0);
	if (codeData == nil) {
		print("devlml: xspanalloc(%lux, %ux, 0)\n", cdsize, BY2PG);
		return;
	}

	print("Installing Motion JPEG driver %s\n", MJPG_VERSION); 
	print("Buffer at 0x%.8lux, size 0x%.8lux\n", codeData, cdsize); 

	// Get access to DMA memory buffer
	memset(codeData, 0xAA, sizeof(CodeData));
	codeData->physaddr = PADDR(codeData->statCom);
	for (i = 0; i < NBUF; i++) {
		codeData->statCom[i] = PADDR(&(codeData->fragdesc[i]));
		codeData->fragdesc[i].addr = PADDR(&(codeData->frag[i]));
		// Length is in double words, in position 1..20
		codeData->fragdesc[i].leng = ((sizeof codeData->frag[i]) >> 1) | FRAGM_FINAL_B;
	}

	print("initializing LML33 board...");

	pciPhysBaseAddr = (void *)(pcidev->mem[0].bar & ~0x0F);

	print("zr36067 found at 0x%.8lux", pciPhysBaseAddr);

	regpa = upamalloc(pcidev->mem[0].bar & ~0x0F, pcidev->mem[0].size, 0);
	if (regpa == 0) {
		print("lml: failed to map registers\n");
		return;
	}
	pciBaseAddr = (ulong)KADDR(regpa);
	print(", mapped at 0x%.8lux\n", pciBaseAddr);

	// Interrupt handler
	intrenable(pcidev->intl, lmlintr, nil, pcidev->tbdf);

	memset(&segbuf, 0, sizeof(segbuf));
	segbuf.attr = SG_PHYSICAL;
	segbuf.name = smalloc(NAMELEN);
	snprint(segbuf.name, NAMELEN, "lmlmjpg");
	segbuf.pa = PADDR(codeData);
	segbuf.size = cdsize;
	if (addphysseg(&segbuf) == -1) {
		print("lml: physsegment: lmlmjpg\n");
		return;
	}

	memset(&segreg, 0, sizeof(segreg));
	segreg.attr = SG_PHYSICAL;
	segreg.name = smalloc(NAMELEN);
	snprint(segreg.name, NAMELEN, "lmlregs");
	segreg.pa = (ulong)regpa;
	segreg.size = pcidev->mem[0].size;
	if (addphysseg(&segreg) == -1) {
		print("lml: physsegment: lmlmjpg\n");
		return;
	}
	return; 
}

static Chan*
lmlattach(char *spec)
{
	return devattach('V', spec);
}

static int
lmlwalk(Chan *c, char *name)
{
	return devwalk(c, name, lmldir, nelem(lmldir), devgen);
}

static void
lmlstat(Chan *c, char *dp)
{
	devstat(c, dp, lmldir, nelem(lmldir), devgen);
}

static Chan*
lmlopen(Chan *c, int omode) {
	int i;

	c->aux = 0;
	switch(c->qid.path){
	case Qjframe:
	case Qjvideo:
	case Qjcount:
		if (nopens)
			error(Einuse);
		nopens = 1;
		singleFrame = (c->qid.path == Qjframe) ? 1 : 0;;
		state = New;
		for (i = 0; i < NBUF; i++)
			codeData->statCom[i] = PADDR(&(codeData->fragdesc[i]));

		// allow one open total for these two
		break;
	}
	return devopen(c, omode, lmldir, nelem(lmldir), devgen);
}

static void
lmlclose(Chan *c) {

	switch(c->qid.path){
	case Qjvideo:
	case Qjframe:
	case Qjcount:
		nopens = 0;
		authclose(c);
	}
}

static long
lmlread(Chan *c, void *va, long n, vlong voff) {
	uchar *buf = va;
	long off = voff;

	switch(c->qid.path & ~CHDIR){

	case Qdir:
		return devdirread(c, (char *)buf, n, lmldir, nelem(lmldir), devgen);
	case Qjvideo:
	case Qjframe:
		return vread(c, buf, n, off);
	case Qjcount:
		return vcount(c, buf, n, off);
	}
}

static long
lmlwrite(Chan *c, void *va, long n, vlong voff) {
	uchar *buf = va;
	long off = voff;

	switch(c->qid.path & ~CHDIR){
	case Qdir:
	case Qjcount:
		error(Eperm);
	case Qjvideo:
	case Qjframe:
		return vwrite(c, buf, n, off);
	}
}

Dev lmldevtab = {
	'V',
	"video",

	lmlreset,
	devinit,
	lmlattach,
	devclone,
	lmlwalk,
	lmlstat,
	lmlopen,
	devcreate,
	lmlclose,
	lmlread,
	devbread,
	lmlwrite,
	devbwrite,
	devremove,
	devwstat,
};

static void
lmlintr(Ureg *, void *) {
	static count;
	ulong flags = readl(pciBaseAddr+INTR_STAT);
	
	if(debug&(DBGINTR))
		print("MjpgDrv_intrHandler stat=0x%.8lux\n", flags);

	// Reset all interrupts from 067
	writel(0xff000000, pciBaseAddr + INTR_STAT);

	if(flags & INTR_JPEGREP) {
		if ((debug&DBGINTR) || ((debug&DBGINTS) && (count++ % 128) == 0))
			print("MjpgDrv_intrHandler wakeup\n");
		wakeup(&sleeper);
	}
	return;
}
