#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"io.h"

#include	"devlml.h"

// Lml 22 driver

enum{
	Q819,
	Q856,
	Qi22,
	Q060,
	Q067,
	Qstat,
	Qvideo,
	Qjframe,
};

static Dirtab viddir[]={
//	 name,		 qid,	  size,		mode
	"vid819",	{Q819},		0,		0644,
	"vid856",	{Q856},		0,		0644,
	"vidi22",	{Qi22},		0,		0644,
	"vid060",	{Q060},		0,		0644,
	"vid067",	{Q067},		0,		0644,
	"vidstat",	{Qstat},	0,		0444,
	"video",	{Qvideo},	0,		0666,
	"jframe",	{Qjframe},	0,		0666,
};

CodeData *	codeData;
MjpgDrv *	mjpgDrv;

static void lmlintr(Ureg *ur, void *arg);

static void
vidreset(void)
{
	ulong regpa;
	int i;

	codeData = (CodeData*)xspanalloc(sizeof(CodeData), BY2PG, 0);
	if (codeData == nil) {
		print("devlml: xspanalloc(%ux, %ux, 0)\n", sizeof(CodeData), BY2PG);
		return;
	}

	print("Installing Motion JPEG driver %s\n", MJPG_VERSION); 
	print("Buffer size %ux\n", sizeof(CodeData)); 

	// Get access to DMA memory buffer
	memset(codeData, 0xAA, sizeof(CodeData));
	strncpy(codeData->idString, MJPG_VERSION, strlen(MJPG_VERSION));

	for(i = 0; i < NBUF; i++) {
		codeData->statCom[i] = PADDR(&(codeData->fragmDescr[i]));
		codeData->statComInitial[i] = codeData->statCom[i];
		codeData->fragmDescr[i].fragmAddress =
			(H33_Fragment *)PADDR(&(codeData->frag[i]));
		// Length is in double words, in position 1..20
		codeData->fragmDescr[i].fragmLength = (FRAGSIZE >> 1) | FRAGM_FINAL_B;
	}

	// Get dynamic kernel memory allocaton for the driver
	if((mjpgDrv = xallocz(sizeof(MjpgDrv), 0)) == nil) {
		print("LML33: can't allocate dynamic memory for MjpgDrv\n");
		return;
	}
	if((lml33Board = xallocz(sizeof(LML33Board), 0)) == nil) {
		print("LML33: can't allocate dynamic memory for lml33Board\n");
		return;
	}

	print("initializing LML33 board...");

	lml33Board->pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (lml33Board->pcidev == nil) {
		print("zr36067 not found. Install aborted.\n");
		return;
	}
	lml33Board->pciPhysBaseAddr =
		(void *)(lml33Board->pcidev->mem[0].bar & ~0x0F);

	print("zr36067 found at %lux\n", lml33Board->pciPhysBaseAddr);

	regpa = upamalloc(lml33Board->pcidev->mem[0].bar & ~0x0F, lml33Board->pcidev->mem[0].size, 0);
	if (regpa == 0) {
		print("lml: failed to map registers\n");
		return;
	}
	lml33Board->pciBaseAddr = KADDR(regpa);

	// make sure the device will respond to mem accesses
	// (pcicmd_master | pcicmd_memory) -- probably superfluous
//	pcicfgw32(lml33Board->pcidev, PciPCR, 0x04 | 0x02);

	// set bus latency -- probably superfluous
//	pcicfgw8(lml33Board->pcidev, PciLTR, 64);

	// Interrupt handler
	intrenable(lml33Board->pcidev->intl, lmlintr, lml33Board, lml33Board->pcidev->tbdf);

	print("LML33 Installed\n"); 
	return; 
}

static Chan*
vidattach(char *spec)
{
	return devattach('V', spec);
}

static int
vidwalk(Chan *c, char *name)
{
	return devwalk(c, name, viddir, nelem(viddir), devgen);
}

static void
vidstat(Chan *c, char *dp)
{
	devstat(c, dp, viddir, nelem(viddir), devgen);
}

static Chan*
vidopen(Chan *c, int omode)
{
	c->aux = 0;
	switch(c->qid.path){
	case Q819:
	case Q856:
	case Qi22:
	case Q060:
	case Q067:
		// allow one open per file
		break;
	case Qstat:
		// allow many opens
		break;
	case Qvideo:
	case Qjframe:
		// allow one open total for these two
		break;
	}
	return devopen(c, omode, viddir, nelem(viddir), devgen);
}

static void
vidclose(Chan *c)
{
	switch(c->qid.path){
	case Q819:
	case Q856:
	case Qi22:
	case Q060:
	case Q067:
	case Qstat:
	case Qvideo:
	case Qjframe:
		authclose(c);
	}
}

static long
vidread(Chan *c, void *buf, long n, vlong off)
{
	switch(c->qid.path){
	case Q819:
	case Q856:
	case Qi22:
	case Q060:
	case Q067:
		return chipread(c, buf, n, off);
	case Qstat:
		return statread(c, buf, n, off);
	case Qvideo:
	case Qjframe:
		return videoread(c, buf, n, off);
	}
}

static long
vidwrite(Chan *c, void *va, long n, vlong off)
{
}

Dev viddevtab = {
	'V',
	"video",

	vidreset,
	devinit,
	vidattach,
	devclone,
	vidwalk,
	vidstat,
	vidopen,
	devcreate,
	vidclose,
	vidread,
	devbread,
	vidwrite,
	devbwrite,
	devremove,
	devwstat,
};

static void
lmlintr(Ureg *ur, void *arg)
{
	LML33Board *lml33Board = (Lml33Board *)arg;

	
}
