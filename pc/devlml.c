#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"io.h"

#include	"devlml.h"

#define DBGREGS 0x1
#define DBGREAD 0x2
#define DBGWRIT 0x4
int debug = DBGREAD|DBGWRIT|DBGREGS;

// Lml 22 driver

struct {
	ulong pci;
	ulong statcom;
	ulong codedata;
} lmlmap;

enum{
	Qdir,
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
	"lml819",	{Q819},		0,		0644,
	"lml856",	{Q856},		0,		0644,
	"lmlreg",	{Qreg},		0x400,		0644,
	"lmlmap",	{Qmap},		sizeof lmlmap,	0444,
	"lmlbuf",	{Qbuf},		0,		0644,
	"jvideo",	{Qjvideo},	0,		0666,
	"jframe",	{Qjframe},	0,		0666,
};

CodeData *	codeData;

int		currentBuffer;
int		currentBufferLength;
void *		currentBufferPtr;
int		frameNo;
Rendez		sleeper;
int		singleFrame;
int		bufferPrepared;
int		hdrPos;
int		nopens;
uchar		q856[3];

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
	ulong a;

	for(i=0;;i++) {
		a = readl(pciBaseAddr + ZR36057_I2C_BUS);
		if (a & ZR36057_I2C_SCL) break;
		if (i>I2C_TIMEOUT) error(Eio);
	}
}

static void
i2c_start(void) {

	writel(ZR36057_I2C_SCL|ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();

	writel(0, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();
}

static void
i2c_stop(void) {
	// the clock should already be low, make sure data is
	writel(0, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();

	// set clock high and wait for device to catch up
	writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	// set the data high to indicate the stop bit
	writel(ZR36057_I2C_SCL|ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();
}

static void i2c_wrbit(int bit) {
	if (bit){
		writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS); // set data
		i2c_pause();
		writel(ZR36057_I2C_SDA|ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
		i2c_waitscl();
		i2c_pause();
		writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
		i2c_pause();
	} else {
		writel(0, pciBaseAddr + ZR36057_I2C_BUS); // clr data
		i2c_pause();
		writel(ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
		i2c_waitscl();
		i2c_pause();
		writel(0, pciBaseAddr + ZR36057_I2C_BUS);
		i2c_pause();
	}
}

static int
i2c_rdbit(void) {
	int bit;
	// the clk line should be low

	// ensure we are not asserting the data line
	writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();

	// set the clock high and wait for device to catch up
	writel(ZR36057_I2C_SDA|ZR36057_I2C_SCL, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_waitscl();
	i2c_pause();

	// the data line should be a valid bit
	bit = readl(pciBaseAddr+ZR36057_I2C_BUS);
	if (bit & ZR36057_I2C_SDA){
		bit = 1;
	} else {
		bit = 0;
	}

	// set the clock low to indicate end of cycle
	writel(ZR36057_I2C_SDA, pciBaseAddr + ZR36057_I2C_BUS);
	i2c_pause();

	return bit;
}

static int
i2c_rdbyte(uchar *v) {
	int i, ack;
	uchar res;

	res = 0;
	for (i=0;i<8;i++){
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

	for (i=0;i<8;i++){
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

	for(i=0;i<num;i+=1){
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
getBuffer(int i, void** bufferPtr, int* frameNo) {

	if(codeData->statCom[i] & STAT_BIT) {
		*bufferPtr = (void*)(&codeData->frag[i]);
		*frameNo = codeData->statCom[i] >> 24;
		return (codeData->statCom[i] & 0x00FFFFFF) >> 1;
	} else
		return -1;
}

static long
vread(Chan *, void *va, long count, vlong pos) {
	int prevFrame;
	//  how much bytes left to transfer for the header
	int hdrLeft;
	// Count of bytes that we need to copy into buf from code-buffer
	// (different from count only while in header reading mode)
	int cpcount = count;
	// Count of bytes that we copied into buf altogether and will return
	int retcount=0;
	vlong thetime;
	uchar *buf = va;

	if(debug&DBGREAD)
		pprint("devlml::vread() count=%ld pos=%lld\n", count, pos);

	// If we just begin reading a file, pos would never be 0 otherwise
	if (pos == 0 && hdrPos == -1) {
		if(debug&DBGREAD)
			pprint("devlml::first read\n");
		 currentBuffer = -1;
		 currentBufferLength = 0;
		 frameNo = -1;
	}
	prevFrame = frameNo;

	// We get to the end of the current buffer, also covers just
	// open file, since 0 >= -1
	if(hdrPos == -1 && pos >= currentBufferLength) {
		if(debug&DBGREAD)
			pprint("devlml::prepareBuffer\n");
		prepareBuffer(currentBuffer);
		// if not the first buffer read and single frame mode - return EOF
		if (currentBuffer != -1 && singleFrame)
			return 0;
		if(debug&DBGREAD)
			pprint("devlml::sleep\n");
		while((currentBuffer = getProcessedBuffer()) == -1)
			sleep(&sleeper, return0, 0);
		if(debug&DBGREAD)
			pprint("devlml::wokeup\n");
		currentBufferLength = getBuffer(currentBuffer,
			&currentBufferPtr, &frameNo);

		pos = 0; // ??????????????

		// print("getBufffer %d -> %d 0x%x %d\n",currentBuffer, currentBufferLength, currentBufferPtr, frameNo);
		if(frameNo != (prevFrame + 1) % 256)
			print("Frames out of sequence: %d %d\n", prevFrame, frameNo);
		// Fill in APP marker fields here
		thetime = todget();
		frameHeader.sec = (ulong)(thetime / 1000000000LL);
		frameHeader.usec = (ulong)(thetime % 1000000000LL) / 1000;
		frameHeader.frameSize = currentBufferLength - 2 + sizeof(FrameHeader);
		frameHeader.frameSeqNo++;
		frameHeader.frameNo = frameNo;
		hdrPos=0;
	}

	if (hdrPos != -1) {
		hdrLeft = sizeof(FrameHeader) - hdrPos;
		// Write the frame size here
		if (count >= hdrLeft) {
			memmove(buf, (char*)&frameHeader + hdrPos, hdrLeft);
			retcount += hdrLeft;
			cpcount = count - hdrLeft;
			pos = sizeof(frameHeader.mrkSOI);
			hdrPos = -1;
		} else {
			memmove(buf, (char*)&frameHeader + hdrPos, count);
			hdrPos += count;
			return count;
		}
	}

	if(cpcount + pos > currentBufferLength)
		cpcount = currentBufferLength - pos;

	memmove(buf + retcount, (char *)currentBufferPtr + pos, cpcount);
	retcount += cpcount;

	//pr_debug&DBGREGS("return %d %d\n",cpcount,retcount);
	return retcount;
}

static long
vwrite(Chan *, void *va, long count, vlong pos) {
	//  how much bytes left to transfer for the header
	int hdrLeft;
	char *buf = va;

	//print("devlml::vwrite() count=0x%x pos=0x%x\n", count, pos);

	// We just started writing, not into the header copy
	if(pos==0 && hdrPos == -1) {
		 currentBuffer=-1;
		 currentBufferLength=0;
		 frameNo=-1;
		 bufferPrepared = 0;
	}

	// We need next buffer to fill (either because we're done with the
	// current buffer) of because we're just beginning (but not into the header)
	if (hdrPos == -1 && pos >= currentBufferLength) {
		while((currentBuffer = getProcessedBuffer()) == -1)
			sleep(&sleeper, return0, 0);
		// print("current buffer %d\n",currentBuffer);

		getBuffer(currentBuffer, &currentBufferPtr, &frameNo);
		// We need to receive the header now
		hdrPos = 0;
	}
	
	// We're into the header processing 
	if (hdrPos != -1) {
		// Calculate how many bytes we need to receive to fill the header
		hdrLeft = sizeof(FrameHeader) - hdrPos;
	
		// If we complete or go over the header with this count
		if (count >= hdrLeft) {
			// Adjust count of bytes that remain to be copied into video buffer
			count = count - hdrLeft; 
			memmove((char*)&frameHeader + hdrPos, buf, hdrLeft);
			// Make sure we have a standard LML33 header
			if (frameHeader.mrkSOI == MRK_SOI
			 && frameHeader.mrkAPP3 == MRK_APP3
			 && strcmp(frameHeader.nm, APP_NAME) == 0) {
				//print("Starting new buffer len=0x%x frame=%d\n", frameHeader.frameSize, frameHeader.frameSeqNo);
				// Obtain values we need for playback process from the header
				currentBufferLength = frameHeader.frameSize;
			} else if (singleFrame) {
				currentBufferLength = FRAGSIZE;
			} else {
				// We MUST have header for motion video decompression
				print("No frame size (APP3 marker) in MJPEG file\n");
				error(Eio);
			}
			// Finish header processing
			hdrPos = -1;
			// Copy the header into the playback buffer
			memmove(currentBufferPtr, (char*)&frameHeader, sizeof(FrameHeader));
			// And set position just behind header for playback buffer write
			pos = sizeof(FrameHeader);
		} else {
			memmove((char*)&frameHeader + hdrPos, buf, count);
			hdrPos += count;
			return count;
		}
	} else
		hdrLeft = 0;

	if(count + pos > currentBufferLength) {
		count = currentBufferLength - pos;
	}

	memmove((char *)currentBufferPtr + pos, buf + hdrLeft, count);

	pos += count;
	// print("return 0x%x 0x%x\n",pos,count);

	// Now is the right moment to initiate playback of the frame (if it's full)
	if(pos >= currentBufferLength) {
		// We have written the frame, time to display it
		//print("Passing written buffer to 067\n");
		prepareBuffer(currentBuffer);
		bufferPrepared = 1;
	}
	//print("return 0x%lx 0x%x 0x%x 0x%x\n",pos,count,hdrLeft+count,currentBufferLength);

	return hdrLeft + count;
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
	for(i = 0; i < NBUF; i++) {
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

	c->aux = 0;
	switch(c->qid.path){
	case Q819:
	case Q856:
	case Qreg:
	case Qmap:
	case Qbuf:
		break;
	case Qjvideo:
	case Qjframe:
		if (nopens)
			error(Einuse);
		nopens = 1;
		singleFrame = (c->qid.path == Qjframe) ? 1 : 0;;
		currentBuffer = 0;
		currentBufferLength = 0;
		currentBufferPtr = 0;
		frameNo = 0;
		bufferPrepared = 0;
		hdrPos = -1;

		// allow one open total for these two
		break;
	}
	return devopen(c, omode, lmldir, nelem(lmldir), devgen);
}

static void
lmlclose(Chan *c) {

	switch(c->qid.path){
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

	case Q819:
		if (off < 0 || off + n >= 0x20)
			return 0;
		for (i = n; i > 0; i--)
			if (i2c_wr8(BT819Addr, off++, *buf++) == 0)
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
	ulong flags = readl(pciBaseAddr+ZR36057_INTR_STAT);
	
	if(debug&(DBGREAD|DBGWRIT))
		print("MjpgDrv_intrHandler stat=0x%.8lux\n", flags);

	// Reset all interrupts from 067
	writel(0xff000000, pciBaseAddr + ZR36057_INTR_STAT);

	if(flags & ZR36057_INTR_JPEGREP) {
		if(debug&(DBGREAD|DBGWRIT))
			print("MjpgDrv_intrHandler wakeup\n");
		wakeup(&sleeper);
	}
	return;
}
