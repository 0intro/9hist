#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

/* flash partitions */
typedef struct FlashPart FlashPart;
struct FlashPart
{
	QLock;
	char	name[NAMELEN];
	ulong	start;		/* byte offsets */
	ulong	end;
};

static ulong *flash = (ulong*)FLASHZERO;

/*
 *  on the bitsy, all 32 bit accesses to flash are mapped to two 16 bit
 *  accesses, one to the low half of the chip and the other to the high
 *  half.  Therefore for all command accesses, ushort indices in the
 *  manuals turn into ulong indices in our code.  Also, by copying all
 *  16 bit commands to both halves of a 32 bit command, we erase 2
 *  sectors for each request erase request.
 */

/*
 *  common flash memory interface
 */
enum
{
	CFIidoff=	0x10,
	CFIsysoff=	0x1B,
	CFIgeomoff=	0x27,
};

struct CFIid
{
	ulong	q;
	ulong	r;
	ulong	y;
	ulong	cmd_set;
	ulong	vendor_alg;
	ulong	ext_alg_addr[2];
	ulong	alt_cmd_set;
	ulong	alt_vendor_alg;
	ulong	alt_ext_ald_addr[2];
	
};

struct CFIsys
{
	ulong 	vcc_min;	/* 100 mv */
	ulong	vcc_max;
	ulong	vpp_min;
	ulong	vpp_max;
	ulong	word_wr_to;		/* 2**n µs */
	ulong	buf_wr_to;		/* 2**n µs */
	ulong	block_erase_to;		/* 2**n ms */
	ulong	chip_erase_to;		/* 2**n ms */
	ulong	max_word_wr_to;		/* 2**n µs */
	ulong	max_buf_wr_to;		/* 2**n µs */
	ulong	max_block_erase_to;	/* 2**n ms */
	ulong	max_chip_erase_to;	/* 2**n ms */
};

struct CFIgeom
{
	ulong	size;		/* 2**n bytes */
	ulong	
};
