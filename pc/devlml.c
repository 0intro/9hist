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
	Q060,
	Q067,
	Qvideo,
	Qjframe,
};

static Dirtab viddir[]={
//	 name,		 qid,	  size,		mode
	"vid819",	{Q819},		0,		0644,
	"vid856",	{Q856},		0,		0644,
	"vid060",	{Q060},		0,		0644,
	"vid067",	{Q067},		0,		0644,
	"video",	{Qvideo},	0,		0666,
	"jframe",	{Qjframe},	0,		0666,
};

CodeData *	codeData;
MjpgDrv *	mjpgDrv;

static void
lml33_i2c_pause(void) {

	microdelay(I2C_DELAY);
}

static void
lml33_i2c_waitscl(void) {
	int i;
	ulong a;

	for(i=0;;i++) {
		a = readl(pciBaseAddr + ZR36057_I2C_BUS);
		if (a & ZR36057_I2C_SCL) break;
		if (i>I2C_TIMEOUT) error(Eio);
	}
}

static void
lml33_i2c_start(void) {

	writel(ZR36057_I2C_SCL|ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_waitscl();
	lml33_i2c_pause();

	writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();

	writel(0, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();
}

static void
lml33_i2c_stop(void) {
	// the clock should already be low, make sure data is
	writel(0, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();

	// set clock high and wait for device to catch up
	writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_waitscl();
	lml33_i2c_pause();

	// set the data high to indicate the stop bit
	writel(ZR36057_I2C_SCL|ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();
}

static void lml33_i2c_wrbit(int bit) {
	if (bit){
		writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS); // set data
		lml33_i2c_pause();
		writel(ZR36057_I2C_SDA|ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
		lml33_i2c_waitscl();
		lml33_i2c_pause();
		writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
		lml33_i2c_pause();
	} else {
		writel(0, pciBaseAddr + ZR36057_I2C_BUS); // clr data
		lml33_i2c_pause();
		writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
		lml33_i2c_waitscl();
		lml33_i2c_pause();
		writel(0, pciBaseAddr + ZR36057_I2C_BUS);
		lml33_i2c_pause();
	}
}

static int
lml33_i2c_rdbit(void) {
	int bit;
	// the clk line should be low

	// ensure we are not asserting the data line
	writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();

	// set the clock high and wait for device to catch up
	writel(ZR36057_I2C_SDA|ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_waitscl();
	lml33_i2c_pause();

	// the data line should be a valid bit
	bit = readl(pciBaseAddr+ZR36057_I2C_BUS);
	if (bit & ZR36057_I2C_SDA){
		bit = 1;
	} else {
		bit = 0;
	}

	// set the clock low to indicate end of cycle
	writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	lml33_i2c_pause();

	return bit;
}

static int
lml33_i2c_rdbyte(uchar *v) {
	int i, ack;
	uchar res;

	res = 0;
	for (i=0;i<8;i++){
		res  = res << 1;
		res += lml33_i2c_rdbit();
	}

	ack = lml33_i2c_rdbit();

	*v = res;

	return ack;
}

static int
lml33_i2c_wrbyte(uchar v) {
	int i, ack;

	for (i=0;i<8;i++){
		lml33_i2c_wrbit(v & 0x80);
		v = v << 1;
	}

	ack = lml33_i2c_rdbit();

	return ack;
}

static void
lml33_i2c_write_bytes(uchar addr, uchar sub, uchar *bytes, long num) {
	int ack;
	long i;

	lml33_i2c_start();

	ack = lml33_i2c_wrbyte(addr);
	if (ack == 1) error(Eio);

	ack = lml33_i2c_wrbyte(sub);
	if (ack == 1) error(Eio);

	for(i=0;i<num;i+=1){
		ack = lml33_i2c_wrbyte(bytes[i]);
		if (ack == 1) error(Eio);
	}

	lml33_i2c_stop();
}

static uchar
lml33_i2c_rd8(int addr, int sub)
{
	int ack;
	uchar msb;

	lml33_i2c_start();

	ack = lml33_i2c_wrbyte(addr);
	if (ack == 1){
		lml33_i2c_stop();
		error(Eio);
	}

	ack = lml33_i2c_wrbyte(sub);
	if (ack == 1){
		lml33_i2c_stop();
		error(Eio);
	}

	lml33_i2c_start();

	ack = lml33_i2c_wrbyte(addr+1);
	if (ack == 1){
		lml33_i2c_stop();
		error(Eio);
	}

	ack = lml33_i2c_rdbyte(&msb);
	if (ack == 0){
		lml33_i2c_stop();
		error(Eio);
	}

	lml33_i2c_stop();

	return msb;
}

/*
 * The following mapping applies for the guests in the LML33
 *
 * Guest        Device
 *   0          zr36060
 *              uses subaddress GADR[0..1]
 *   1          zr36060 START#
 *   2          -
 *   3          zr36060 RESET#
 *   4          -
 *   5          -
 *   6          -
 *   7          -
 */

// lml33_post_idle waits for the guest bus to become free
static int
lml33_post_idle(void) {
	ulong a;
	int timeout;

	for(timeout = 0;;timeout += 1){
		a = readl(pciBaseAddr + ZR36057_POST_OFFICE);
		if ((a & ZR36057_POST_PEND) == 0) 
			return a;
		if (timeout == GUEST_TIMEOUT) 
			return -1;
	}
}

// lml33_post_write writes a byte to a guest using postoffice mechanism
static void
lml33_post_write(int guest, int reg, int v) {
	int w;

	// wait for postoffice not busy
	lml33_post_idle();

	// Trim the values, just in case
	guest &= 0x07;
	reg   &= 0x07;
	v     &= 0xFF;

	// write postoffice operation
	w = ZR36057_POST_DIR + (guest<<20) + (reg<<16) + v;
	writel(w, pciBaseAddr + ZR36057_POST_OFFICE);

	// wait for postoffice not busy
	w = lml33_post_idle();

	// decide if write went ok
	if (w == -1) error(Eio);
}

// lml33_post_read reads a byte from a guest using postoffice mechanism
static uchar
lml33_post_read(int guest, int reg) {
	int w;

	// wait for postoffice not busy
	lml33_post_idle();

	// Trim the values, just in case
	guest &= 0x07;
	reg   &= 0x07;

	// write postoffice operation
	w = (guest<<20) + (reg<<16);
	writel(w, pciBaseAddr + ZR36057_POST_OFFICE);

	// wait for postoffice not busy, get result
	w = lml33_post_idle();

	// decide if read went ok
	if (w == -1) error(Eio);

	return (uchar)(w & 0xFF);
}

static void
lml33_zr060_write(int reg, int v) {
	int guest_id;

	guest_id = GID060;

	lml33_post_write(guest_id, 1, reg>>8 & 0x03);
	lml33_post_write(guest_id, 2, reg    & 0xff);
	lml33_post_write(guest_id, 3, v);
}

static uchar
lml33_zr060_read(int reg) {
	int guest_id;

	guest_id = GID060;

	lml33_post_write(guest_id, 1, reg>>8 & 0x03);
	lml33_post_write(guest_id, 2, reg    & 0xff);

	return lml33_post_read(guest_id, 3);
}

long
chipread(long addr, char *buf, long n, long off) {
	long i;

	for (i = 0; i < n; i++) {
		*buf++ = lml33_i2c_rd8(addr, off++);
	}
	return i;
}

long
post060read(char *buf, long n, long off) {
	long i;

	for (i = 0; i < n; i++) {
		*buf++ = lml33_zr060_read(off++);
	}
	return i;
}

static void lmlintr(Ureg *, void *);

static void
vidreset(void)
{
	ulong regpa;
	int i;

	pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (pcidev == nil) {
		print("No zr36067 found.\n");
		return;
	}
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
			(Fragment *)PADDR(&(codeData->frag[i]));
		// Length is in double words, in position 1..20
		codeData->fragmDescr[i].fragmLength = (FRAGSIZE >> 1) | FRAGM_FINAL_B;
	}

	// Get dynamic kernel memory allocaton for the driver
	if((mjpgDrv = xallocz(sizeof(MjpgDrv), 0)) == nil) {
		print("LML33: can't allocate dynamic memory for MjpgDrv\n");
		return;
	}

	print("initializing LML33 board...");

	pciPhysBaseAddr = (void *)(pcidev->mem[0].bar & ~0x0F);

	print("zr36067 found at %lux\n", pciPhysBaseAddr);

	regpa = upamalloc(pcidev->mem[0].bar & ~0x0F, pcidev->mem[0].size, 0);
	if (regpa == 0) {
		print("lml: failed to map registers\n");
		return;
	}
	pciBaseAddr = (ulong)KADDR(regpa);

	// make sure the device will respond to mem accesses
	// (pcicmd_master | pcicmd_memory) -- probably superfluous
//	pcicfgw32(pcidev, PciPCR, 0x04 | 0x02);

	// set bus latency -- probably superfluous
//	pcicfgw8(pcidev, PciLTR, 64);

	// Interrupt handler
	intrenable(pcidev->intl, lmlintr, nil, pcidev->tbdf);

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
	case Q060:
	case Q067:
		// allow one open per file
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
	case Q060:
	case Q067:
	case Qvideo:
	case Qjframe:
		authclose(c);
	}
}

static long
vidread(Chan *c, void *buf, long n, vlong off) {

	switch(c->qid.path){
	case Q819:
		if (off < 0 || off + n > 20)
			return 0;
		return chipread(BT819Addr, buf, n, off);
	case Q856:
		if (off < 0xda || off + n > 0xe0)
			return 0;
		return chipread(BT856Addr, buf, n, off);
	case Q060:
		return post060read(buf, n, off);
	case Q067:
		if (off < 0 || off + n > 20 || (off & 0x3) || n != 4) return 0;
		*(long *)buf = readl(pciBaseAddr + off);
		return 4;
	case Qvideo:
	case Qjframe:
		return videoread(c, buf, n, off);
	}
}

static long
vidwrite(Chan *c, void *va, long n, vlong off)
{

	switch(c->qid.path){
	case Q819:
		if (off < 0 || off + n > 20)
			return 0;
		return chipwrite(BT819Addr, buf, n, off);
	case Q856:
		if (off < 0xda || off + n > 0xe0)
			return 0;
		return chipwrite(BT856Addr, buf, n, off);
	case Q060:
		return post060write(buf, n, off);
	case Q067:
		if (off < 0 || off + n > 20 || (off & 0x3) || n != 4) return 0;
		writel(*(long *)buf, pciBaseAddr + off);
		return 4;
	case Qvideo:
	case Qjframe:
		return videowrite(c, buf, n, off);
	}
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
lmlintr(Ureg *ur, void *)
{


}
