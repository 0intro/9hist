// Lml 22 driver

#define MJPG_VERSION "LML33 v0.2"

// Various minor numbers (functions) of the device
#define MJPG_MINOR_STATUS 0
#define MJPG_MINOR_VIDEO 1
#define MJPG_MINOR_FRAME 2
#define MJPG_MINOR_STILL 3

// The following values can be modified to tune/set default behaviour of the
// driver.

// The number of uS delay in I2C state transitions
#define H33_I2C_DELAY 10

// The amount of spinning to do before the I2C bus is timed out
#define H33_I2C_TIMEOUT 10000000

// The amount of spinning to do before the guest bus is timed out
#define H33_GUEST_TIMEOUT 10000000

// The amount of spinning to do before the polling of the still
// transfer port is aborted.
#define H33_STILL_TIMEOUT 1000000

// The following number is the maximum number of cards permited. Each
// card found is mapped to a device minor number starting from 0.
#define H33_MAX_CARDS 1

// The following is the number of device types supported.
#define H33_DEVICE_COUNT 2

// The number of 8K pages per buffer, we will allocate four buffers,
// locked into memory whenever the device is open so modify with care.
#define H33_PAGES 32

// The following are the datastructures needed by the device.

// A H33_Device records the properties of the various card types supported.
typedef struct {
	int		number;			// The H33_CARDTYPE_ assigned
	char	*card_name;		// A string name
	int		zr060addr;		// Which guest bus address for the ZR36060
} H33_Device;

// An entry in the fragment table
typedef struct {
	ulong	address;		// bus address of page
	int		length;			// length of page
} H33_RingPage;

// The structure that we will use to tell the '57 about the buffers
// The sizeof(H33_RingData) should not exceed page size
typedef struct {
	void			*buffer[4];
	ulong			i_stat_com[4];
	H33_RingPage	ring_pages[4][H33_PAGES];
} H33_RingData;

typedef struct {
	int		expect;			// the buffer the int routine expects next
	int		which;			// which ring buffer the read or write uses
	int		filled;			// the current number of filled buffers
	int		pages;			// the number of complete pages
	int		remainder;		// the number of bytes in incomplete page
} H33_RingPtr;

// The remainder of the #defs are constants which should not need changing.

// The PCI vendor and device ids of the zoran chipset on the dc30
// these really belong in pci.h
#define PCI_VENDOR_ZORAN				0x11de
#define PCI_DEVICE_ZORAN_36057			0x6057

// The ZR36057 is mapped into a 4Kbyte block of the address space
// starting at PCI_BASE_ADDRESS_0. Its application specific registers
// are layed out within that space as follows.
#define ZR36057_VFEND_HORCON			0x000
#define ZR36057_VFEND_VERCON			0x004
#define ZR36057_VFEND_SCLPIX			0x008
#define ZR36057_VDISP_TOP				0x00c
#define ZR36057_VDISP_BOT				0x010
#define ZR36057_VSTRD_GRB				0x014
#define ZR36057_VDISP_CONF				0x018
#define ZR36057_MASK_TOP				0x01c
#define ZR36057_MASK_BOT				0x020
#define ZR36057_OVRLY_CTL				0x024
#define ZR36057_SYS_PCI					0x028
#define ZR36057_GPIO_CTL				0x02c
#define ZR36057_MPEG_SRCW				0x030
#define ZR36057_MPEG_CTFR_CTL			0x034
#define ZR36057_MPEG_MEM_PTR			0x038
#define ZR36057_INTR_STAT				0x03c
#define ZR36057_INTR_CTL				0x040
#define ZR36057_I2C_BUS					0x044
#define ZR36057_JPEG_MODE_CTL			0x100
#define ZR36057_JPEG_PROC_CTL			0x104
#define ZR36057_VSYNC_PARM				0x108
#define ZR36057_HSYNC_PARM				0x10c
#define ZR36057_HOR_ACTIVE				0x110
#define ZR36057_VER_ACTIVE				0x114
#define ZR36057_FIELD_PROC				0x118
#define ZR36057_JPEG_CODE_BASE			0x11C
#define ZR36057_JPEG_FIFO_THRHLD		0x120
#define ZR36057_JPEG_GUESTID			0x124
#define ZR36057_GUEST_CTL				0x12c
#define ZR36057_POST_OFFICE				0x200
#define ZR36057_STILL_TRANS				0x300
// The datasheet says that STILL_TRANS is 0x140, but the errata says it's 0x300

// which bits of the ZR36057_INTR... mean something special
#define ZR36057_INTR_GIRQ1              0x40000000
#define ZR36057_INTR_GIRQ0              0x20000000
#define ZR36057_INTR_CODREP             0x10000000
#define ZR36057_INTR_JPEGREP            0x08000000
#define ZR36057_INTR_ENABLE             0x01000000


// which bits of ZR36057_I2C_BUS mean something special
#define ZR36057_POST_PEND               0x02000000
#define ZR36057_POST_TIME               0x01000000
#define ZR36057_POST_DIR                0x00800000

// function of the Guest CS outputs
#define ZR36060_GUEST_DATA              0
#define ZR36060_GUEST_START             1
#define ZR36060_GUEST_RESET             3

// the still busy bit in ZR36057_STILL_TRANS
// this assumes we are working in little endian mode
#define ZR36057_STILL_BUSY              0x80000000

// which bit of ZR36057_I2C_BUS is which
#define ZR36057_I2C_SCL                 1
#define ZR36057_I2C_SDA                 2

// The ZR36060 registers
#define ZR36060_LOAD                    0x000
#define ZR36060_FIFO_STAT               0x001
#define ZR36060_INTERFACE               0x002
#define ZR36060_MODE                    0x003
#define ZR36060_ZERO                    0x004
#define ZR36060_MBCV                    0x005
#define ZR36060_MARKERS_EN              0x006
#define ZR36060_INT_MASK                0x007
#define ZR36060_INT_STAT                0x008
#define ZR36060_TCV_NEThi               0x009
#define ZR36060_TCV_NETmh               0x00a
#define ZR36060_TCV_NETml               0x00b
#define ZR36060_TCV_NETlo               0x00c
#define ZR36060_TCV_DATAhi              0x00d
#define ZR36060_TCV_DATAmh              0x00e
#define ZR36060_TCV_DATAml              0x00f
#define ZR36060_TCV_DATAlo              0x010
#define ZR36060_SFhi                    0x011
#define ZR36060_SFlo                    0x012
#define ZR36060_AFhi                    0x013
#define ZR36060_AFme                    0x014
#define ZR36060_AFlo                    0x015
#define ZR36060_ACVhi                   0x016
#define ZR36060_ACVmh                   0x017
#define ZR36060_ACVml                   0x018
#define ZR36060_ACVlo                   0x019
#define ZR36060_ATAhi                   0x01a
#define ZR36060_ATAmh                   0x01b
#define ZR36060_ATAml                   0x01c
#define ZR36060_ATAlo                   0x01d
#define ZR36060_ACV_TRUNhi              0x01e
#define ZR36060_ACV_TRUNmh              0x01f
#define ZR36060_ACV_TRUNml              0x020
#define ZR36060_ACV_TRUNlo              0x021
#define ZR36060_DEV_ID                  0x022
#define ZR36060_DEV_REV                 0x023
#define ZR36060_TEST_1                  0x024
#define ZR36060_TEST_2                  0x025
#define ZR36060_VCR                     0x030
#define ZR36060_VPR                     0x031
#define ZR36060_SCALE                   0x032
#define ZR36060_BKG_CLR_Y               0x033
#define ZR36060_BKG_CLR_U               0x034
#define ZR36060_BKG_CLR_V               0x035
#define ZR36060_SYNC_VTOTALhi           0x036
#define ZR36060_SYNC_VTOTALlo           0x037
#define ZR36060_SYNC_HTOTALhi           0x038
#define ZR36060_SYNC_HTOTALlo           0x039
#define ZR36060_SYNC_VSIZE              0x03a
#define ZR36060_SYNC_HSIZE              0x03b
#define ZR36060_SYNC_BVSTART            0x03c
#define ZR36060_SYNC_BHSTART            0x03d
#define ZR36060_SYNC_BVENDhi            0x03e
#define ZR36060_SYNC_BVENDlo            0x03f
#define ZR36060_SYNC_BHENDhi            0x040
#define ZR36060_SYNC_BHENDlo            0x041
#define ZR36060_AA_VSTARThi             0x042
#define ZR36060_AA_VSTARTlo             0x043
#define ZR36060_AA_VENDhi               0x044
#define ZR36060_AA_VENDlo               0x045
#define ZR36060_AA_HSTARThi             0x046
#define ZR36060_AA_HSTARTlo             0x047
#define ZR36060_AA_HENDhi               0x048
#define ZR36060_AA_HENDlo               0x049
#define ZR36060_SW_VSTARThi             0x04a
#define ZR36060_SW_VSTARTlo             0x04b
#define ZR36060_SW_VENDhi               0x04c
#define ZR36060_SW_VENDlo               0x04d
#define ZR36060_SW_HSTARThi             0x04e
#define ZR36060_SW_HSTARTlo             0x04f
#define ZR36060_SW_HENDhi               0x050
#define ZR36060_SW_HENDlo               0x051

#define MB 0x100000
#define NBUF 4

#define FRAGM_FINAL_B 1
#define STAT_BIT 1

typedef struct MjpgDrv				MjpgDrv;
typedef struct H33_Fragment			H33_Fragment;
typedef struct H33_FragmentTable	H33_FragmentTable;
typedef struct CodeData				CodeData;
typedef struct ML33Board			LML33Board;

#define FRAGSIZE (MB/NBUF)

struct H33_Fragment {
	uchar	fragbytes[FRAGSIZE];
};

struct H33_FragmentTable {
	H33_Fragment *	fragmAddress;			// Physical address
	ulong			fragmLength;
};

struct CodeData {
	char				idString[16];
	ulong				statCom[4];			// Physical address
	ulong				statComInitial[4];	// Physical address
	H33_FragmentTable	fragmDescr[4];
	H33_Fragment		frag[4];
};

extern char static_MjpgDrv_GPL_Notice[];
extern struct file_operations static_MjpgDrv_fOps;

struct MjpgDrv {
	int					openCount;
	int					sleepFlag;
	struct wait_queue *	intrWaitQ;
};

#define PCI_DEVICE_ZORAN_36067 PCI_DEVICE_ZORAN_36057

struct ML33Board
{
	void *		pciPhysBaseAddr;
	void *		pciBaseAddr;
	Pcidev *	pcidev;
};

extern void *		H33Addr;
extern LML33Board *	lml33Board;

void	LML33Board_ctor(LML33Board*);
void	LML33Board_dtor(LML33Board*);
int		LML33Board_installInterruptHandler(LML33Board*);
void	LML33Board_initHardware(LML33Board*);
void	LML33Board_mjpegGo(LML33Board*);


// ZR36067 (PCI controller) register memory access
ulong	LML33Board_readL(LML33Board*,int addr);
void	LML33Board_writeL(LML33Board*,int addr,ulong);
ushort	LML33Board_readW(LML33Board*,int addr);
void	LML33Board_writeW(LML33Board*,int addr,ushort);
uchar	LML33Board_readB(LML33Board*,int addr);
void	LML33Board_writeB(LML33Board*,int addr,uchar);
int		LML33Board_getBit(LML33Board*,int addr,int);
void	LML33Board_setBit(LML33Board*,int addr,int,int);

// I2C (video encoder/decoder) manipulation functions
void	h33_i2c_pause(LML33Board*);
int		h33_i2c_waitscl(LML33Board*);
void	h33_i2c_start(LML33Board*);
void	h33_i2c_stop(LML33Board*);
void	h33_i2c_wrbit(LML33Board*,int bit);
int		h33_i2c_rdbit(LML33Board*);
int		h33_i2c_wrbyte(LML33Board*,int value);
int		h33_i2c_rdbyte(LML33Board*,int* value);
int		h33_i2c_probe(LML33Board*,int addr);
int		h33_i2c_wr8(LML33Board*,int addr, int sub, int value);
int		h33_i2c_rd8(LML33Board*,int addr, int sub, int *value);
int		h33_i2c_bt856rd8 (LML33Board*,int addr, int *msb);

// GPIO access
void	gpioSetDirection(int gpIO,int dir);
void	gpioSet(int gpIO,int value);
int		gpioGet(int gpIO);

// PostOffice access functions 
int		h33_post_idle(void);
int		h33_post_write(int guest, int reg, int value);
int		h33_post_read(int guest, int reg);

// ZR36060
void	h33_zr060_write(int reg, int value);
int		h33_zr060_read(int reg);

void	MjpgDrv_ctor(MjpgDrv*);
void	MjpgDrv_dtor(MjpgDrv*);
int		MjpgDrv_open(struct inode *iNode, struct file *filePtr);
void	MjpgDrv_release(struct inode *iNode, struct file *filePtr);
void	MjpgDrv_intrHandler(int irqNo, void *devId, struct pt_regs *ptRegs);

void *	static_CodeData_map(ulong h33PHighMemory,ulong h33BufferSize);
int		CodeData_getReadyBuffer(CodeData*);
int		CodeData_getProcessedBuffer(CodeData*this);
int		CodeData_getBuffer(CodeData *,int bufferNo,void** bufferPtr,ushort* frameNo);
int		CodeData_prepareBuffer(CodeData *, int bufferNo);
