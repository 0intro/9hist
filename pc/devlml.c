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

#define DBGREGS 0x01
#define DBGREAD 0x02
#define DBGWRIT 0x04
#define DBG819	0x08
#define DBGINTR	0x10
#define DBGINTS	0x20

int debug = 0;

// Lml 22 driver

struct {
	ulong pci;
	ulong statcom;
	ulong codedata;
} lmlmap;

enum{
	Qdir,
	Q060,
	Q819,
	Q856,
	Qreg,
	Qmap,
	Qbuf,
	Qjvideo,
	Qjframe,
};

static Dirtab lmldir[]={
//	 name,		 qid,		size,		mode
	"lml060",	{Q060},		0x400,		0644,
	"lml819",	{Q819},		0,		0644,
	"lml856",	{Q856},		0,		0644,
	"lmlreg",	{Qreg},		0x400,		0644,
	"lmlmap",	{Qmap},		sizeof lmlmap,	0444,
	"lmlbuf",	{Qbuf},		0,		0644,
	"jvideo",	{Qjvideo},	0,		0666,
	"jframe",	{Qjframe},	0,		0666,
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

ulong
writel(ulong v, ulong a) {
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) <-- %.8lux\n",
			a, (ulong)a-pciBaseAddr, v);
	return *(ulong *)a = v;
}

ushort
writew(ushort v, ulong a) {
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) <--     %.4ux\n",
			a, (ulong)a-pciBaseAddr, v);
	return *(ushort *)a = v;
}

uchar
writeb(uchar v, ulong a) {
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) <--       %.2ux\n",
			a, (ulong)a-pciBaseAddr, v);
	return *(uchar *)a = v;
}

ulong
readl(ulong a) {
	ulong v;

	v = *(ulong*)a;
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) --> %.8lux\n",
			a, (ulong)a-pciBaseAddr, v);
	return v;
}

ushort
readw(ulong a) {
	ushort v;

	v = *(ushort*)a;
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) -->     %.4ux\n",
			a, (ulong)a-pciBaseAddr, v);
	return v;
}

uchar
readb(ulong a) {
	uchar v;

	v = *(uchar*)a;
	if (debug&DBGREGS)
		pprint("%.8lux (%.8lux) -->       %.2ux\n",
			a, (ulong)a-pciBaseAddr, v);
	return v;
}

static void
i2c_pause(void) {

	microdelay(I2C_DELAY);
}

static void
i2c_waitscl(void) {
	int i;

	for (i = 0; i < I2C_TIMEOUT; i++)
		if (readl(pciBaseAddr + I2C_BUS) & I2C_SCL)
			return;
	error(Eio);
}

static void
i2c_start(void) {

	writel(I2C_SCL|I2C_SDA, pciBaseAddr + I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	writel(I2C_SCL, pciBaseAddr + I2C_BUS);
	i2c_pause();

	writel(0, pciBaseAddr + I2C_BUS);
	i2c_pause();
}

static void
i2c_stop(void) {
	// the clock should already be low, make sure data is
	writel(0, pciBaseAddr + I2C_BUS);
	i2c_pause();

	// set clock high and wait for device to catch up
	writel(I2C_SCL, pciBaseAddr + I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	// set the data high to indicate the stop bit
	writel(I2C_SCL|I2C_SDA, pciBaseAddr + I2C_BUS);
	i2c_pause();
}

static void i2c_wrbit(int bit) {
	if (bit){
		writel(I2C_SDA, pciBaseAddr + I2C_BUS); // set data
		i2c_pause();
		writel(I2C_SDA|I2C_SCL, pciBaseAddr + I2C_BUS);
		i2c_waitscl();
		i2c_pause();
		writel(I2C_SDA, pciBaseAddr + I2C_BUS);
		i2c_pause();
	} else {
		writel(0, pciBaseAddr + I2C_BUS); // clr data
		i2c_pause();
		writel(I2C_SCL, pciBaseAddr + I2C_BUS);
		i2c_waitscl();
		i2c_pause();
		writel(0, pciBaseAddr + I2C_BUS);
		i2c_pause();
	}
}

static int
i2c_rdbit(void) {
	int bit;
	// the clk line should be low

	// ensure we are not asserting the data line
	writel(I2C_SDA, pciBaseAddr + I2C_BUS);
	i2c_pause();

	// set the clock high and wait for device to catch up
	writel(I2C_SDA|I2C_SCL, pciBaseAddr + I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	// the data line should be a valid bit
	bit = readl(pciBaseAddr+I2C_BUS);
	if (bit & I2C_SDA){
		bit = 1;
	} else {
		bit = 0;
	}

	// set the clock low to indicate end of cycle
	writel(I2C_SDA, pciBaseAddr + I2C_BUS);
	i2c_pause();

	return bit;
}

static int
i2c_rdbyte(uchar *v) {
	int i, ack;
	uchar res;

	res = 0;
	for (i = 0; i < 8; i++) {
		res  = res << 1;
		res += i2c_rdbit();
	}

	ack = i2c_rdbit();

	*v = res;

	return ack;
}

static int
i2c_wrbyte(uchar v) {
	int i, ack;

	for (i = 0; i < 8; i++) {
		i2c_wrbit(v & 0x80);
		v = v << 1;
	}

	ack = i2c_rdbit();

	return ack;
}

static void
i2c_write_bytes(uchar addr, uchar sub, uchar *bytes, long num) {
	int ack;
	long i;

	i2c_start();

	ack = i2c_wrbyte(addr);
	if (ack == 1) error(Eio);

	ack = i2c_wrbyte(sub);
	if (ack == 1) error(Eio);

	for (i = 0; i < num; i++) {
		ack = i2c_wrbyte(bytes[i]);
		if (ack == 1) error(Eio);
	}

	i2c_stop();
}

static int
i2c_bt856rd8(void) {
	uchar ret;

	i2c_start ();

	if (i2c_wrbyte(BT856Addr + 1) == 1
	 || i2c_rdbyte(&ret) == 0) {
		i2c_stop ();
		return -1;
	}

	i2c_stop ();
	return ret;
}

static int
i2c_rd8(int addr, int sub)
{
	uchar msb;

	i2c_start();

	if (i2c_wrbyte(addr) == 1
	 || i2c_wrbyte(sub) == 1) {
		if (debug&DBGREGS) pprint("i2c_rd8, failure 1\n");
		i2c_stop();
		return -1;
	}

	i2c_start();

	if (i2c_wrbyte(addr+1) == 1
	 || i2c_rdbyte(&msb) == 0){
		if (debug&DBGREGS) pprint("i2c_rd8, failure 2\n");
		i2c_stop();
		return -1;
	}

	i2c_stop();

	return msb;
}

static int
i2c_wr8(uchar addr, uchar sub, uchar msb) {
	
	i2c_start();

	if (i2c_wrbyte(addr) == 1
	 || i2c_wrbyte(sub) == 1
	 || i2c_wrbyte(msb) == 1)
		return 0;
	
	i2c_stop();
	
	return 1;
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

// post_idle waits for the guest bus to become free
static int
post_idle(void) {
	ulong a;
	int timeout;

	for (timeout = 0; timeout < GUEST_TIMEOUT; timeout++){
		a = readl(pciBaseAddr + POST_OFFICE);
		if ((a & POST_PEND) == 0) 
			return a;
	}
	pprint("post_idle timeout\n");
	return -1;
}

// post_write writes a byte to a guest using postoffice mechanism
int
post_write(unsigned int guest, unsigned int reg, unsigned int v) {
	int w;

	// wait for postoffice not busy
	post_idle();

	// Trim the values, just in case
	guest &= 0x07;
	reg   &= 0x07;
	v     &= 0xFF;

	// write postoffice operation
	w = POST_DIR | (guest<<20) | (reg<<16) | v;
	writel(w, pciBaseAddr + POST_OFFICE);

	// wait for postoffice not busy
	return post_idle() == -1;
}

// post_read reads a byte from a guest using postoffice mechanism
int
post_read(int guest, int reg) {
	int w;

	// wait for postoffice not busy
	post_idle();

	// Trim the values, just in case
	guest &= 0x07;
	reg   &= 0x07;

	// write postoffice operation
	w = (guest<<20) + (reg<<16);
	writel(w, pciBaseAddr + POST_OFFICE);

	// wait for postoffice not busy, get result
	w = post_idle();

	// decide if read went ok
	if (w == -1) return -1;

	return w & 0xFF;
}

static int
zr060_write(int reg, int v) {
  
	if (post_write(GID060, 1, (reg>>8) & 0x03)
	 || post_write(GID060, 2, reg & 0xff)
	 || post_write(GID060, 3, v))
		return -1;
}

static int
zr060_read(int reg) {
  
	if (post_write(GID060, 1, (reg>>8) & 0x03)
	 || post_write(GID060, 2, reg    & 0xff))
		return -1;
	return post_read(GID060, 3);
}

static int
prepareBuffer(int i) {
	if (i >= 0 && i < NBUF && (codeData->statCom[i] & STAT_BIT)) {
		codeData->statCom[i] = PADDR(&(codeData->fragdesc[i]));
    		return codeData->fragdesc[i].leng;
	} else
		return -1;
}

static int
getProcessedBuffer(void){
	static lastBuffer = NBUF-1;
	int l = lastBuffer;

	while (1) { 
		lastBuffer = (lastBuffer+1) % NBUF;
		if (codeData->statCom[lastBuffer] & STAT_BIT)
			return lastBuffer;
		if (lastBuffer == l)
			break;
	}
	return -1;
}

static int
getBuffer(int i, int* frameNo) {

	if(codeData->statCom[i] & STAT_BIT) {
		*frameNo = codeData->statCom[i] >> 24;
		return (codeData->statCom[i] & 0x00FFFFFF) >> 1;
	} else
		return -1;
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
			while ((curbuf = getProcessedBuffer()) == -1)
				sleep(&sleeper, return0, 0);
			fragsize = getBuffer(curbuf, &frameno);
			if (debug & DBGREAD)
				pprint("devlml: got read buf %d, fr %d, size %d\n",
					curbuf, frameNo, fragsize);
			if (frameno != (frameprev + 1) % 256)
				pprint("Frame out of sequence: %d %d\n",
					frameprev, frameno);
			frameprev = frameno;
			if (fragsize <= 0 || fragsize > sizeof(Fragment)) {
				pprint("Wrong sized fragment, %d (ignored)\n",
					fragsize);
				prepareBuffer(curbuf);
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
			prepareBuffer(curbuf);
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
			while((curbuf = getProcessedBuffer()) == -1) {
				if (debug&DBGWRIT)
					pprint("devlml::sleep\n");
				sleep(&sleeper, return0, 0);
			}
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
			i = prepareBuffer(curbuf);
			if (debug&DBGWRIT)
				pprint("Sending buffer %d: %d\n", curbuf, i);
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
	ulong regpa;
	int i;

	pcidev = pcimatch(nil, PCI_VENDOR_ZORAN, PCI_DEVICE_ZORAN_36067);
	if (pcidev == nil) {
		return;
	}
	codeData = (CodeData*)xspanalloc(sizeof(CodeData), BY2PG, 0);
	if (codeData == nil) {
		print("devlml: xspanalloc(%ux, %ux, 0)\n", sizeof(CodeData), BY2PG);
		return;
	}

	print("Installing Motion JPEG driver %s\n", MJPG_VERSION); 
	print("Buffer at 0x%.8lux, size 0x%.8ux\n", codeData, sizeof(CodeData)); 

	// Get access to DMA memory buffer
	memset(codeData, 0xAA, sizeof(CodeData));
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

	// make sure the device will respond to mem accesses
	// (pcicmd_master | pcicmd_memory) -- probably superfluous
//	pcicfgw32(pcidev, PciPCR, 0x04 | 0x02);

	// set bus latency -- probably superfluous
//	pcicfgw8(pcidev, PciLTR, 64);

	// Interrupt handler
	intrenable(pcidev->intl, lmlintr, nil, pcidev->tbdf);

	lmlmap.pci = pciBaseAddr;
	lmlmap.statcom = PADDR(codeData->statCom);
	lmlmap.codedata = (ulong)codeData;

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
	case Q060:
	case Q819:
	case Q856:
	case Qreg:
	case Qmap:
	case Qbuf:
		break;
	case Qjframe:
	case Qjvideo:
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
	case Q060:
	case Q819:
	case Q856:
	case Qreg:
	case Qmap:
	case Qbuf:
		authclose(c);
		break;
	case Qjvideo:
	case Qjframe:
		nopens = 0;
		authclose(c);
	}
}

static long
lmlread(Chan *c, void *va, long n, vlong voff) {
	int i, d;
	uchar *buf = va;
	long off = voff;

	switch(c->qid.path & ~CHDIR){

	case Qdir:
		return devdirread(c, (char *)buf, n, lmldir, nelem(lmldir), devgen);

	case Q060:
		if (off < 0 || off + n > 0x400)
			return 0;
		for (i = 0; i < n; i++) {
			if ((d = zr060_read(off + i)) < 0) break;
			*buf++ = d;
		}
		return i;
	case Q819:
		if (off < 0 || off + n > 0x20)
			return 0;
		for (i = 0; i < n; i++) {
			if ((d = i2c_rd8(BT819Addr, off++)) < 0) break;
			*buf++ = d;
		}
		return i;
	case Q856:
		if (n != 1)
			return 0;
		switch ((int)off) {
		case 0:
			if ((d = i2c_bt856rd8()) < 0)
				return 0;
			*buf = d;
			break;
		case 0xDA:
			*buf = q856[0];
			break;
		case 0xDC:
			*buf = q856[1];
			break;
		case 0xDE:
			*buf = q856[2];
			break;
		default:
			return 0;
		}
		return 1;
	case Qmap:
		if (off < 0)
			return 0;
		for (i = 0; i < n; i++) {
			if (off + i >= sizeof lmlmap)
				break;
			buf[i] = ((uchar *)&lmlmap)[off + i];
		}
		return i;
	case Qreg:
		if (off < 0 || off + n >= 0x400)
			return 0;
		switch(n) {
		case 1:
			*buf = readb(pciBaseAddr + off);
			break;
		case 2:
			if (off & (n-1)) return 0;
			*(short *)buf = readw(pciBaseAddr + off);
			break;
		case 4:
			if (off & (n-1)) return 0;
			*(long *)buf = readl(pciBaseAddr + off);
			break;
		default:
			return 0;
		}
		return n;
	case Qbuf:
		if (off < 0 || off + n >= sizeof *codeData)
			return 0;
		switch(n) {
		case 1:
			*buf = readb((ulong)codeData + off);
			break;
		case 2:
			if (off & (n-1)) return 0;
			*(short *)buf = readw((ulong)codeData + off);
			break;
		case 4:
			if (off & (n-1)) return 0;
			*(long *)buf = readl((ulong)codeData + off);
			break;
		default:
			return 0;
		}
		return n;
	case Qjvideo:
	case Qjframe:
		return vread(c, buf, n, off);
	}
}

static long
lmlwrite(Chan *c, void *va, long n, vlong voff) {
	int i;
	uchar *buf = va;
	long off = voff;

	switch(c->qid.path & ~CHDIR){

	case Qdir:
		error(Eperm);

	case Q060:
		if (off < 0 || off + n > 0x400)
			return 0;
		for (i = 0; i < n; i++) {
			if (zr060_write(off + i, *buf++) < 0) break;
		}
		return i;
	case Q819:
		if (off < 0 || off + n >= 0x20)
			return 0;
		for (i = 0; i < n; i++)
			if (i2c_wr8(BT819Addr, off + i, *buf++) == 0)
				break;
		return i;
	case Q856:
		if (n != 1 || off < 0xda || off + n > 0xe0)
			return 0;
		if (i2c_wr8(BT856Addr, off, *buf) == 0)
				return 0;
		switch ((int)off) {
		case 0xDA:
			q856[0] = *buf;
			break;
		case 0xDC:
			q856[1] = *buf;
			break;
		case 0xDE:
			q856[2] = *buf;
			break;
		}
		return 1;
	case Qreg:
		if (off < 0 || off + n >= 0x400)
			return 0;
		switch (n) {
		case 1:
			writeb(*buf, pciBaseAddr + off);
			break;
		case 2:
			if (off & 0x1)
				return 0;
			writew(*(short *)buf, pciBaseAddr + off);
			break;
		case 4:
			if (off & 0x3)
				return 0;
			writel(*(long *)buf, pciBaseAddr + off);
			break;
		default:
			return 0;
		}
		return n;
	case Qbuf:
		if (off < 0 || off + n >= sizeof *codeData)
			return 0;
		switch (n) {
		case 1:
			writeb(*buf, (ulong)codeData + off);
			break;
		case 2:
			if (off & 0x1)
				return 0;
			writew(*(short *)buf, (ulong)codeData + off);
			break;
		case 4:
			if (off & 0x3)
				return 0;
			writel(*(long *)buf, (ulong)codeData + off);
			break;
		default:
			return 0;
		}
		return n;
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
