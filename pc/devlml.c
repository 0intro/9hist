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

int			currentBuffer;
int			currentBufferLength;
void *		currentBufferPtr;
int			frameNo;
Rendez		sleeper;
int			singleFrame;
int			bufferPrepared;
int			hdrPos;
int			nopens;

static FrameHeader frameHeader = {
	MRK_SOI, MRK_APP3, (sizeof(FrameHeader)-4) << 8,
	{ 'L', 'M', 'L', '\0'},
	-1, 0, 0, 0, 0
};

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

static int
lml33_i2c_rd8(int addr, int sub)
{
	uchar msb;

	lml33_i2c_start();

	if (lml33_i2c_wrbyte(addr) == 1
	 || lml33_i2c_wrbyte(sub) == 1) {
		lml33_i2c_stop();
		return -1;
	}

	lml33_i2c_start();

	if (lml33_i2c_wrbyte(addr+1) == 1
	 || lml33_i2c_rdbyte(&msb) == 0){
		lml33_i2c_stop();
		return -1;
	}

	lml33_i2c_stop();

	return msb;
}

static int
lml33_i2c_wr8(uchar addr, uchar sub, uchar msb) {
	
	lml33_i2c_start();

	if (lml33_i2c_wrbyte(addr) == 1
	 || lml33_i2c_wrbyte(sub) == 1
	 || lml33_i2c_wrbyte(msb) == 1)
		return 0;
	
	lml33_i2c_stop();
	
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
static int
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
	return lml33_post_idle() == -1;
}

// lml33_post_read reads a byte from a guest using postoffice mechanism
static int
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
	if (w == -1) return -1;

	return w & 0xFF;
}

static int
lml33_zr060_write(int reg, int v) {
	int guest_id;

	guest_id = GID060;

	lml33_post_write(guest_id, 1, reg>>8 & 0x03);
	lml33_post_write(guest_id, 2, reg    & 0xff);
	return lml33_post_write(guest_id, 3, v);
}

static int
lml33_zr060_read(int reg) {
	int guest_id;

	guest_id = GID060;

	lml33_post_write(guest_id, 1, reg>>8 & 0x03);
	lml33_post_write(guest_id, 2, reg    & 0xff);

	return lml33_post_read(guest_id, 3);
}

static int
prepareBuffer(CodeData * this, int bufferNo) {
  if(bufferNo >= 0 && bufferNo < NBUF && (this->statCom[bufferNo] & STAT_BIT)) {
    this->statCom[bufferNo] = this->statComInitial[bufferNo];
    return this->fragmDescr[bufferNo].fragmLength;
  } else
    return -1;
}

static int
getProcessedBuffer(CodeData* this){
	static lastBuffer=NBUF-1;
	int lastBuffer0 = lastBuffer;

	while (1) { 
		lastBuffer = (lastBuffer+1) % NBUF;
		if(this->statCom[lastBuffer]&STAT_BIT)
			return lastBuffer;
		if(lastBuffer==lastBuffer0)
			break;
	}
	return -1;
}

static int
getBuffer(CodeData *this, int bufferNo, void** bufferPtr, int* frameNo) {
	int codeLength;
	if(this->statCom[bufferNo] & STAT_BIT) {
		*bufferPtr = (void*)this->fragmDescr[bufferNo].fragmAddress;
		*frameNo = this->statCom[bufferNo] >> 24;
		codeLength=((this->statCom[bufferNo] & 0x00FFFFFF) >> 1);
		return codeLength;
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

	//print("devlml::vread() count=%ld pos=%lld\n", count, pos);

	// If we just begin reading a file, pos would never be 0 otherwise
	if (pos == 0 && hdrPos == -1) {
		 currentBuffer = -1;
		 currentBufferLength = 0;
		 frameNo = -1;
	}
	prevFrame = frameNo;

	// We get to the end of the current buffer, also covers just
	// open file, since 0 >= -1
	if(hdrPos == -1 && pos >= currentBufferLength) {
		prepareBuffer(codeData, currentBuffer);
		// if not the first buffer read and single frame mode - return EOF
		if (currentBuffer != -1 && singleFrame)
			return 0;
		while((currentBuffer = getProcessedBuffer(codeData)) == -1)
			sleep(&sleeper, return0, 0);
		currentBufferLength = getBuffer(codeData, currentBuffer,
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

	//pr_debug("return %d %d\n",cpcount,retcount);
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
		while((currentBuffer = getProcessedBuffer(codeData)) == -1)
			sleep(&sleeper, return0, 0);
		// print("current buffer %d\n",currentBuffer);

		getBuffer(codeData, currentBuffer, &currentBufferPtr, &frameNo);
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
			 && frameHeader.mrkAPP3==MRK_APP3
			 && strcmp(frameHeader.nm,APP_NAME) == 0) {
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
		prepareBuffer(codeData, currentBuffer);
		bufferPrepared = 1;
	}
	//print("return 0x%lx 0x%x 0x%x 0x%x\n",pos,count,hdrLeft+count,currentBufferLength);

	return hdrLeft + count;
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
		break;
	case Qvideo:
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
vidread(Chan *c, void *va, long n, vlong off) {
	int i, d;
	uchar *buf = va;

	switch(c->qid.path){
	case Q819:
		if (off < 0 || off + n > 0x20)
			return 0;
		for (i = 0; i < n; i++) {
			if ((d = lml33_i2c_rd8(BT819Addr, off++)) < 0) break;
			*buf++ = d;
		}
		return n - i;
	case Q856:
		if (off < 0xda || off + n > 0xe0)
			return 0;
		for (i = 0; i < n; i++) {
			if ((d = lml33_i2c_rd8(BT856Addr, off++)) < 0) break;
			*buf++ = d;
		}
		return n - i;
	case Q060:
		if (off < 0 || off + n > 0x60)
			return 0;
		for (i = 0; i < n; i++) {
			if ((d = lml33_zr060_read(off++)) < 0) break;
			*buf++ = d;
		}
		return n - i;
	case Q067:
		if (off < 0 || off + n > 0x200 || (off & 0x3))
			return 0;
		for (i = n; i >= 4; i -= 4) {
			*(long *)buf = readl(pciBaseAddr + off);
			buf += 4;
			off += 4;
		}
		return n-i;
	case Qvideo:
	case Qjframe:
		return vread(c, buf, n, off);
	}
}

static long
vidwrite(Chan *c, void *va, long n, vlong off) {
	int i;
	uchar *buf = va;

	switch(c->qid.path){
	case Q819:
		if (off < 0 || off + n > 0x20)
			return 0;
		for (i = n; i > 0; i--)
			if (lml33_i2c_wr8(BT819Addr, off++, *buf++) == 0)
				break;
		return n - i;
	case Q856:
		if (off < 0xda || off + n > 0xe0)
			return 0;
		for (i = n; i > 0; i--)
			if (lml33_i2c_wr8(BT856Addr, off++, *buf++) == 0)
				break;
		return n - i;
	case Q060:
		if (off < 0 || off + n > 0x60)
			return 0;
		for (i = 0; i < n; i++)
			if (lml33_zr060_write(off++, *buf++) < 0)
				break;
		return n - i;
	case Q067:
		if (off < 0 || off + n > 0x200 || (off & 0x3))
			return 0;
		for (i = n; i >= 4; i -= 4) {
			writel(*(long *)buf, pciBaseAddr + off);
			buf += 4;
			off += 4;
		}
		return n-i;
	case Qvideo:
	case Qjframe:
		return vwrite(c, buf, n, off);
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
lmlintr(Ureg *, void *) {
	ulong flags = readl(pciBaseAddr+ZR36057_INTR_STAT);
	
//  print("MjpgDrv_intrHandler stat=0x%08x\n", flags);

	// Reset all interrupts from 067
	writel(0xff000000, pciBaseAddr + ZR36057_INTR_STAT);

	if(flags & ZR36057_INTR_JPEGREP)
			wakeup(&sleeper);
	return;
}
