typedef struct Thwack		Thwack;
typedef struct Unthwack		Unthwack;
typedef struct ThwBlock		ThwBlock;
typedef struct UnthwBlock	UnthwBlock;

enum
{
	ThwMaxBlock	= 1600,		/* max size of compressible block */

	MinMatch	= 3,		/* shortest match possible */
	HashLog		= 10,
	HashSize	= 1<<HashLog,
	HashMask	= HashSize - 1,

	MaxLen		= 8,
	MaxOff		= 8,
	OffBase		= 6,

	MinDecode	= 9,		/* minimum bits to decode a match or lit */

	EWinBlocks	= 32,		/* blocks held in encoder window */
	DWinBlocks	= 32,		/* blocks held in decoder window */
	CompBlocks	= 5,		/* max blocks used to encode data */

	MaxSeqMask	= 8,		/* number of bits in coding block mask */
	MaxSeqStart	= 256,		/* max offset of initial coding block */
};

struct ThwBlock
{
	ulong	seq;			/* sequence number for this data */
	uchar	acked;			/* ok to use this block; the decoder has it */
	ushort	begin;			/* time of first byte in hash */
	uchar	*edata;			/* last byte of valid data */
	ushort	maxoff;			/* time of last valid hash entry */
	ushort	*hash;
	uchar	*data;
};

struct Thwack
{
	ulong		nbits;		/* output bit buffer */
	ulong		bits;
	uchar		*dst;		/* output buffer */
	uchar		*dmax;

	ulong		slot;		/* next block to use */
	ThwBlock	blocks[EWinBlocks];
	ushort		hash[EWinBlocks][HashSize];
	uchar		data[EWinBlocks][ThwMaxBlock];
};

struct UnthwBlock
{
	ulong	seq;			/* sequence number for this data */
	ushort	maxoff;			/* valid data in each block */
	uchar	*data;
};

struct Unthwack
{
	ulong		nbits;		/* input bit buffer */
	ulong		bits;
	uchar		*src;		/* input buffer */
	uchar		*smax;

	int		slot;		/* next block to use */
	UnthwBlock	blocks[DWinBlocks];
	uchar		data[DWinBlocks][ThwMaxBlock];
};

void	thwackinit(Thwack*);
void	unthwackinit(Unthwack*);
int	thwack(Thwack*, uchar *dst, uchar *src, int nsrc, ulong seq);
void	thwackack(Thwack*, ulong seq, ulong mask);
int	unthwack(Unthwack*, uchar *dst, int ndst, uchar *src, int nsrc, ulong seq);
