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
	ulong	dev_code;	/* ??? */
	ulong	max_multi;	/* max bytes in a multibyte write */
	ulong	nregion;	/* number of erase regions */
	ulong	region[1];	/* erase region info */
};

#define mirror(x) (((x)<<16)|(x))

void
cfiquery(void)
{
	struct CFIid *id;

	flash[0x55] = mirror(0x98);
	id = (struct CFIid*)&flash[0x10];
	if(id.q != 'q' || id.r != 'r' || id.y != 'y')
		print("CFI not supported by flash\n");
	
	flash[0x55] = mirror(0xFF);
}

void
cfigeom(void)
{
}

/*
 *  flash device interface
 */

enum
{
	Qf0=1,
	Qf1,
	Qf2,
	Qf3,
};

Dirtab flashdir[]={
	"f0",		{ Qf0, 0 },	0,	0664,
	"f1",		{ Qf1, 0 },	0,	0664,
	"f2",		{ Qf2, 0 },	0,	0664,
	"f3",		{ Qf3, 0 },	0,	0664,
};

void
flashinit(void)
{
	cfiquery();
	cfigeom();
}

static Chan*
flashattach(char* spec)
{
	return devattach('r', spec);
}

static int	 
flashwalk(Chan* c, char* name)
{
	return devwalk(c, name, flashdir, nelem(flashdir), devgen);
}

static void	 
flashstat(Chan* c, char* dp)
{
	devstat(c, dp, flashdir, nelem(flashdir), devgen);
}

static Chan*
flashopen(Chan* c, int omode)
{
	omode = openmode(omode);
	if(strcmp(up->user, eve)!=0)
		error(Eperm);
	return devopen(c, omode, flashdir, nelem(flashdir), devgen);
}

static void	 
flashclose(Chan*)
{
}

static long	 
flashread(Chan* c, void* a, long n, vlong off)
{
	USED(c, a, off);
	error("UUO");
	return n;
}

static long	 
flashwrite(Chan* c, void* a, long n, vlong)
{
	USED(c, a, off);
	error("UUO");
	return n;
}

Dev flashdevtab = {
	'F',
	"flash",

	devreset,
	flashinit,
	flashattach,
	devclone,
	flashwalk,
	flashstat,
	flashopen,
	devcreate,
	flashclose,
	flashread,
	devbread,
	flashwrite,
	devbwrite,
	devremove,
	devwstat,
};
