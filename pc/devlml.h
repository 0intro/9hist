// Lml 22 driver

#define MJPG_VERSION "LML33 v0.2"

// Various minor numbers (functions) of the device
#define MJPG_MINOR_STATUS 0
#define MJPG_MINOR_VIDEO 1
#define MJPG_MINOR_FRAME 2
#define MJPG_MINOR_STILL 3

// The following values can be modified to tune/set default behaviour of the
// driver.

// The number of uS delay in I2C state transitions (probably >= 10)
#define I2C_DELAY 50

// The amount of spinning to do before the I2C bus is timed out
#define I2C_TIMEOUT 10000000

// The amount of spinning to do before the guest bus is timed out
#define GUEST_TIMEOUT 10000000

// The amount of spinning to do before the polling of the still
// transfer port is aborted.
#define STILL_TIMEOUT 1000000

// The following number is the maximum number of cards permited. Each
// card found is mapped to a device minor number starting from 0.
#define MAX_CARDS 1

// The following is the number of device types supported.
#define DEVICE_COUNT 2

// The number of 8K pages per buffer, we will allocate four buffers,
// locked into memory whenever the device is open so modify with care.
#define PAGES 32

// The following are the datastructures needed by the device.
#define ZR36057_I2C_BUS		0x044
// which bit of ZR36057_I2C_BUS is which
#define ZR36057_I2C_SCL                 1
#define ZR36057_I2C_SDA                 2
#define ZR36057_INTR_JPEGREP            0x08000000
#define ZR36057_INTR_STAT               0x03c

// A Device records the properties of the various card types supported.
typedef struct {
	int		number;			// The H33_CARDTYPE_ assigned
	char	*card_name;		// A string name
	int		zr060addr;		// Which guest bus address for the ZR36060
} Device;

// An entry in the fragment table
typedef struct {
	ulong	address;		// bus address of page
	int		length;			// length of page
} RingPage;

// The structure that we will use to tell the '57 about the buffers
// The sizeof(RingData) should not exceed page size
typedef struct {
	void			*buffer[4];
	ulong			i_stat_com[4];
	RingPage		ring_pages[4][PAGES];
} RingData;

typedef struct {
	int		expect;			// the buffer the int routine expects next
	int		which;			// which ring buffer the read or write uses
	int		filled;			// the current number of filled buffers
	int		pages;			// the number of complete pages
	int		remainder;		// the number of bytes in incomplete page
} RingPtr;

// The remainder of the #defs are constants which should not need changing.

// The PCI vendor and device ids of the zoran chipset on the dc30
// these really belong in pci.h
#define PCI_VENDOR_ZORAN				0x11de
#define PCI_DEVICE_ZORAN_36057			0x6057
#define PCI_DEVICE_ZORAN_36067			PCI_DEVICE_ZORAN_36057

#define BT819Addr 0x8a
#define BT856Addr 0x88

#define MB 0x100000
#define NBUF 4

#define FRAGM_FINAL_B 1
#define STAT_BIT 1

#define writel(v, a)	(*(ulong *)(a) = (v))
#define writew(v, a)	(*(ushort *)(a) = (v))
#define writeb(v, a)	(*(uchar *)(a) = (v))
#define readl(a)		(*(ulong *)(a))
#define readw(a)		(*(ushort *)(a))
#define readb(a)		(*(uchar *)(a))

typedef struct FrameHeader			FrameHeader;
typedef struct MjpgDrv				MjpgDrv;
typedef struct Fragment				Fragment;
typedef struct FragmentTable		FragmentTable;
typedef struct CodeData				CodeData;
typedef struct ML33Board			LML33Board;

#define FRAGSIZE (MB/NBUF)

struct Fragment {
	uchar	fragbytes[FRAGSIZE];
};

struct FragmentTable {	// Don't modify this struct, used by h/w
	Fragment *		fragmAddress;			// Physical address
	ulong			fragmLength;
};

struct CodeData {	// Don't modify this struct, used by h/w
	char			idString[16];
	ulong			statCom[4];				// Physical address
	ulong			statComInitial[4];		// Physical address
	FragmentTable	fragmDescr[4];
	Fragment		frag[4];
};

static void *		pciPhysBaseAddr;
static ulong		pciBaseAddr;
static Pcidev *		pcidev;

//If we're on the little endian architecture, then 0xFF, 0xD8 byte sequence is
#define MRK_SOI		0xD8FF
#define MRK_APP3	0xE3FF
#define APP_NAME	"LML"

struct FrameHeader	// Don't modify this struct, used by h/w
{
	short mrkSOI;
	short mrkAPP3;
	short lenAPP3;
	char nm[4];
	short frameNo;
	ulong sec;
	ulong usec;
	ulong frameSize;
	ulong frameSeqNo;
};
