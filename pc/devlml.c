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

#define DBGREAD	0x01
#define DBGWRIT	0x02
#define DBGINTR	0x04
#define DBGINTS	0x08

int debug = 0;

// Lml 22 driver

enum{
	Qdir,
	Qjpg,
	Qraw,
};

static Dirtab lmldir[]={
	".",		{Qdir, 0, QTDIR},	0,	DMDIR|0555,
	"lmljpg",	{Qjpg},			0,	0444,
	"lmlraw",	{Qraw},			0,	0444,
};

static CodeData *	codeData;

static ulong		jpgframeno;
//static ulong		rawframeno;

//static FrameHeader	rawheader;

static FrameHeader	jpgheader = {
	MRK_SOI, MRK_APP3, (sizeof(FrameHeader)-4) << 8,
	{ 'L', 'M', 'L', '\0'},
	-1, 0, 0,  0
};

int		frameNo;
Rendez	sleepjpg;
//Rendez		sleepraw;
int		singleFrame;
int		jpgopens;
int		jpgmode;
//int		rawopens;

#define writel(v, a) *(ulong *)(a) = (v)
#define readl(a) *(ulong*)(a)

static int
getbuffer(void *){
	static last = NBUF-1;
	int l = last;

	for (;;) {
		last = (last+1) % NBUF;
		if (codeData->statCom[last] & STAT_BIT)
			return last + 1;
		if (last == l)
			return 0;
	}
	return 0;
}

static long
jpgread(Chan *, void *va, long nbytes, vlong, int dosleep) {
	int bufno;
	FrameHeader *jpgheader;

	// reads should be of size 1 or sizeof(FrameHeader)
	// Frameno is the number of the buffer containing the data
	while ((bufno = getbuffer(nil)) == 0 && dosleep)
		sleep(&sleepjpg, getbuffer, 0);
	if (--bufno < 0)
		return 0;

	jpgheader = (FrameHeader*)(codeData->frag[bufno].hdr+2);
	if (nbytes == sizeof(FrameHeader)) {
		memmove(va, jpgheader, sizeof(FrameHeader));
		return sizeof(FrameHeader);
	}
	if (nbytes == 1) {
		*(char *)va = bufno;
		return 1;
	}
	return 0;
}

static void lmlintr(Ureg *, void *);

static void
prepbuf(void) {
	int i;

	for (i = 0; i < NBUF; i++) {
		codeData->statCom[i] = PADDR(&(codeData->fragdesc[i]));
		codeData->fragdesc[i].addr = PADDR(codeData->frag[i].fb);
		// Length is in double words, in position 1..20
		codeData->fragdesc[i].leng = (FRAGSIZE >> 1) | FRAGM_FINAL_B;
		memmove(codeData->frag[i].hdr+2, &jpgheader, sizeof(FrameHeader)-2);
	}
}

static void
lmlreset(void)
{
	Physseg segbuf;
	Physseg segreg;
	Physseg seggrab;
	ulong regpa;
	ulong cdsize;
	void *grabbuf;
	ulong grablen;
	ISAConf isa;

	if(isaconfig("lml", 0, &isa) == 0) {
		if (debug) print("lml not in plan9.ini\n");
		return;
	}
	pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (pcidev == nil) {
		return;
	}

	cdsize = CODEDATASIZE;
	codeData = (CodeData*)(((ulong)xalloc(cdsize+ BY2PG) + BY2PG-1) & ~(BY2PG-1));
	if (codeData == nil) {
		print("devlml: xalloc(%lux, %ux, 0)\n", cdsize, BY2PG);
		return;
	}

	grablen = GRABDATASIZE;
	grabbuf = (void*)(((ulong)xalloc(grablen+ BY2PG) + BY2PG-1) & ~(BY2PG-1));
	if (grabbuf == nil) {
		print("devlml: xalloc(%lux, %ux, 0)\n", grablen, BY2PG);
		return;
	}

	print("Installing Motion JPEG driver %s, irq %d\n", MJPG_VERSION, pcidev->intl); 
	print("MJPG buffer at 0x%.8lux, size 0x%.8lux\n", codeData, cdsize); 
	print("Grab buffer at 0x%.8lux, size 0x%.8lux\n", grabbuf, grablen); 

	// Get access to DMA memory buffer
	codeData->pamjpg = PADDR(codeData->statCom);
	codeData->pagrab = PADDR(grabbuf);

	prepbuf();

	pciPhysBaseAddr = (void *)(pcidev->mem[0].bar & ~0x0F);

	print("zr36067 found at 0x%.8lux", pciPhysBaseAddr);

	regpa = upamalloc(pcidev->mem[0].bar & ~0x0F, pcidev->mem[0].size, 0);
	if (regpa == 0) {
		print("lml: failed to map registers\n");
		return;
	}
	pciBaseAddr = (ulong)KADDR(regpa);
	print(", mapped at 0x%.8lux\n", pciBaseAddr);

	memset(&segbuf, 0, sizeof(segbuf));
	segbuf.attr = SG_PHYSICAL;
	kstrdup(&segbuf.name, "lmlmjpg");
	segbuf.pa = PADDR(codeData);
	segbuf.size = cdsize;
	if (addphysseg(&segbuf) == -1) {
		print("lml: physsegment: lmlmjpg\n");
		return;
	}

	memset(&segreg, 0, sizeof(segreg));
	segreg.attr = SG_PHYSICAL;
	kstrdup(&segreg.name, "lmlregs");
	segreg.pa = (ulong)regpa;
	segreg.size = pcidev->mem[0].size;
	if (addphysseg(&segreg) == -1) {
		print("lml: physsegment: lmlregs\n");
		return;
	}

	memset(&seggrab, 0, sizeof(seggrab));
	seggrab.attr = SG_PHYSICAL;
	kstrdup(&seggrab.name, "lmlgrab");
	seggrab.pa = PADDR(grabbuf);
	seggrab.size = grablen;
	if (addphysseg(&seggrab) == -1) {
		print("lml: physsegment: lmlgrab\n");
		return;
	}

	// Interrupt handler
	intrenable(pcidev->intl, lmlintr, nil, pcidev->tbdf, "lml");

	return;
}

static Chan*
lmlattach(char *spec)
{
	return devattach('V', spec);
}

static Walkqid*
lmlwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, lmldir, nelem(lmldir), devgen);
}

static int
lmlstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, lmldir, nelem(lmldir), devgen);
}

static Chan*
lmlopen(Chan *c, int omode) {

	c->aux = 0;
	switch((ulong)c->qid.path){
	case Qjpg:
	case Qraw:
		// allow one open
		if (jpgopens)
			error(Einuse);
		jpgopens = 1;
		jpgframeno = 0;
		jpgmode = omode;
		prepbuf();
		break;
	}
	return devopen(c, omode, lmldir, nelem(lmldir), devgen);
}

static void
lmlclose(Chan *c) {

	switch((ulong)c->qid.path){
	case Qjpg:
	case Qraw:
		jpgopens = 0;
		break;
	}
}

static long
lmlread(Chan *c, void *va, long n, vlong voff) {
	uchar *buf = va;
	long off = voff;

	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, (char *)buf, n, lmldir, nelem(lmldir), devgen);
	case Qjpg:
		return jpgread(c, buf, n, off, 1);
	case Qraw:
		return jpgread(c, buf, n, off, 0);
	}
}

static long
lmlwrite(Chan *, void *, long, vlong) {

	error(Eperm);
	return 0;
}

Dev lmldevtab = {
	'V',
	"video",

	lmlreset,
	devinit,
	lmlattach,
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
	ulong fstart, fno;
	ulong flags = readl(pciBaseAddr+INTR_STAT);
	FrameHeader *jpgheader;
	
	// Reset all interrupts from 067
	writel(0xff000000, pciBaseAddr + INTR_STAT);

	if(flags & INTR_JPEGREP) {

		if(debug&(DBGINTR))
			print("MjpgDrv_intrHandler stat=0x%.8lux\n", flags);

		fstart = jpgframeno & 0x00000003;
		for (;;) {
			jpgframeno++;
			fno = jpgframeno & 0x00000003;
			if (codeData->statCom[fno] & STAT_BIT)
				break;
			if (fno == fstart) {
				if (debug & DBGINTR)
					print("Spurious lml jpg intr?\n");
				return;
			}
		}
		if (jpgopens && jpgmode == OREAD){
			jpgheader = (FrameHeader *)(codeData->frag[fno].hdr+2);
			jpgheader->frameNo = jpgframeno;
			jpgheader->ftime  = todget(nil);
			jpgheader->frameSize =
				(codeData->statCom[fno] & 0x00ffffff) >> 1;
			jpgheader->frameSeqNo = codeData->statCom[fno] >> 24;
		}
		wakeup(&sleepjpg);
	}
	return;
}
