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
	Qjcount,
};

static Dirtab lmldir[]={
//	 name,		 qid,		size,		mode
	"jcount",	{Qjcount},	0,		0444,
};

CodeData *	codeData;

int		frameNo;
Rendez		sleeper;
int		singleFrame;
int		nopens;

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

static void lmlintr(Ureg *, void *);

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
	int i;

	pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (pcidev == nil) {
		return;
	}
	cdsize = CODEDATASIZE;
	codeData = (CodeData*)xspanalloc(cdsize, BY2PG, 0);
	if (codeData == nil) {
		print("devlml: xspanalloc(%lux, %ux, 0)\n", cdsize, BY2PG);
		return;
	}

	grablen = GRABDATASIZE;
	grabbuf = xspanalloc(grablen, BY2PG, 0);
	if (grabbuf == nil) {
		print("devlml: xspanalloc(%lux, %ux, 0)\n", grablen, BY2PG);
		return;
	}

	print("Installing Motion JPEG driver %s\n", MJPG_VERSION); 
	print("MJPG buffer at 0x%.8lux, size 0x%.8lux\n", codeData, cdsize); 
	print("Grab buffer at 0x%.8lux, size 0x%.8lux\n", grabbuf, grablen); 

	// Get access to DMA memory buffer
	codeData->pamjpg = PADDR(codeData->statCom);
	codeData->pagrab = PADDR(grabbuf);
	for (i = 0; i < NBUF; i++) {
		codeData->statCom[i] = PADDR(&(codeData->fragdesc[i]));
		codeData->fragdesc[i].addr = PADDR(&(codeData->frag[i]));
		// Length is in double words, in position 1..20
		codeData->fragdesc[i].leng = ((sizeof codeData->frag[i]) >> 1) | FRAGM_FINAL_B;
	}

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
		print("lml: physsegment: lmlregs\n");
		return;
	}

	memset(&seggrab, 0, sizeof(seggrab));
	seggrab.attr = SG_PHYSICAL;
	seggrab.name = smalloc(NAMELEN);
	snprint(seggrab.name, NAMELEN, "lmlgrab");
	seggrab.pa = PADDR(grabbuf);
	seggrab.size = grablen;
	if (addphysseg(&seggrab) == -1) {
		print("lml: physsegment: lmlgrab\n");
		return;
	}

	// Interrupt handler
	intrenable(pcidev->intl, lmlintr, nil, pcidev->tbdf);

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

	c->aux = 0;
	switch(c->qid.path){
	case Qjcount:
		// allow one open
		if (nopens)
			error(Einuse);
		nopens = 1;
		break;
	}
	return devopen(c, omode, lmldir, nelem(lmldir), devgen);
}

static void
lmlclose(Chan *c) {

	switch(c->qid.path){
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
	case Qjcount:
		return vcount(c, buf, n, off);
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
