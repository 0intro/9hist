#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

typedef struct Huff	Huff;

typedef struct Decode	Decode;

enum
{
	ZBase		= 2,			/* base of code to encode 0 runs */
	LitBase		= ZBase-1,		/* base of literal values */
	MaxLit		= 256,

	MaxLeaf		= MaxLit+LitBase,
	MaxHuffBits	= 15,			/* max bits in a huffman code */
	MaxFlatbits	= 4,			/* max bits decoded in flat table */

	Nhuffblock	= 16*1024,		/* symbols encoded before output */
	Nhuffslop	= 64,			/* slop for stuffing zeros, etc. */
};

struct Huff
{
	int	minbits;
	int	maxbits;
	int	flatbits;
	ulong	flat[1<<MaxFlatbits];
	ulong	maxcode[MaxHuffBits+1];
	ulong	last[MaxHuffBits+1];
	ulong	decode[MaxLeaf];
};

struct Decode{
	Huff	tab;
	int	ndec;
	int	nbits;
	ulong	bits;
	int	nzero;
	int	base;
	ulong	maxblocksym;

	uchar	*src;				/* input buffer */
	uchar	*smax;				/* limit */
};

static	void	fatal(Decode *dec, char*);

static	int	hdec(Decode*);
static	void	hbflush(Decode*);
static	ulong	bitget(Decode*, int);
static	int	mtf(uchar*, int);

int
unsac(uchar *dst, uchar *src, int n, int nsrc)
{
	Decode *dec;
	uchar *buf, *front;
	ulong *suflink, *sums;
	ulong sum;
	int i, m, I, j, c;

	dec = malloc(sizeof *dec);
	buf = malloc(n+2);
	suflink = malloc((n+2) * sizeof *suflink);
	front = malloc(256 * sizeof *front);
	sums = malloc(256 * sizeof *sums);

	if(waserror()){
		free(dec);
		free(buf);
		free(suflink);
		free(front);
		free(sums);
		nexterror();
	}

	dec->src = src;
	dec->smax = src + nsrc;

	dec->nbits = 0;
	dec->bits = 0;
	dec->nzero = 0;
	for(i = 0; i < 256; i++)
		front[i] = i;

	n++;
	I = bitget(dec, 16);
	if(I >= n)
		fatal(dec, "corrupted input");

	/*
	 * decode the character usage map
	 */
	for(i = 0; i < 256; i++)
		sums[i] = 0;
	c = bitget(dec, 1);
	for(i = 0; i < 256; ){
		m = bitget(dec, 8) + 1;
		while(m--){
			if(i >= 256)
				fatal(dec, "corrupted char map");
			front[i++] = c;
		}
		c = c ^ 1;
	}

	/*
	 * initialize mtf state
	 */
	c = 0;
	for(i = 0; i < 256; i++)
		if(front[i])
			front[c++] = i;
	dec->maxblocksym = c + LitBase;

	/*
	 * huffman decoding, move to front decoding,
	 * along with character counting
	 */
	hbflush(dec);
	for(i = 0; i < n; i++){
		if(i == I)
			continue;
		m = hdec(dec);

		/*
		 * move to front
		 */
		c = front[m];
		for(; m > 0; m--)
			front[m] = front[m-1];
		front[0] = c;

		buf[i] = c;
		sums[c]++;
	}

	sum = 1;
	for(i = 0; i < 256; i++){
		c = sums[i];
		sums[i] = sum;
		sum += c;
	}

	/*
	 * calculate the row step for column step array
	 * by calculating it for backwards moves and inverting it
	 */
	suflink[0] = I;
	for(j = 0; j < I; j++)
		suflink[sums[buf[j]]++] = j;
	for(j++; j < n; j++)
		suflink[sums[buf[j]]++] = j;

	/*
	 * to recover the suffix array, aka suffix array for input
	 * j = 0;
	 * for(i = I; i != 0; i = suflink[i])
	 *	sarray[i] = j++;
	 * sarray[i] = j++;
	 * note that suflink[i] = sarrayinv[sarray[i] + 1]
	 */

	/*
	 * produce the decoded data forwards
	 */
	n--;
	i = I;
	for(j = 0; j < n; j++){
		i = suflink[i];
		dst[j] = buf[i];
	}

	poperror();
	free(dec);
	free(buf);
	free(suflink);
	free(front);
	free(sums);
	return n;
}

static ulong
bitget(Decode *dec, int nb)
{
	int c;

	while(dec->nbits < nb){
		if(dec->src >= dec->smax)
			fatal(dec, "premature eof 1");
		c = *dec->src++;
		dec->bits <<= 8;
		dec->bits |= c;
		dec->nbits += 8;
	}
	dec->nbits -= nb;
	return (dec->bits >> dec->nbits) & ((1 << nb) - 1);
}

static void
fillbits(Decode *dec)
{
	int c;

	while(dec->nbits < 24){
		if(dec->src >= dec->smax)
			fatal(dec, "premature eof 2");
		c = *dec->src++;
		dec->bits <<= 8;
		dec->bits |= c;
		dec->nbits += 8;
	}
}

/*
 * decode one symbol
 */
static int
hdecsym(Decode *dec, Huff *h)
{
	long c;
	int b;

	dec->bits &= (1 << dec->nbits) - 1;
	for(b = h->flatbits; (c = dec->bits >> (dec->nbits - b)) > h->maxcode[b]; b++)
		;
	if(b > h->maxbits)
		fatal(dec, "too many bits consumed");
	dec->nbits -= b;
	c = h->decode[h->last[b] - c];

	return c;
}

static int
hdec(Decode *dec)
{
	ulong c;

	dec->ndec++;
	if(dec->nzero){
		dec->nzero--;
		return 0;
	}

	if(dec->nbits < dec->tab.maxbits)
		fillbits(dec);
	dec->bits &= (1 << dec->nbits) - 1;
	c = dec->tab.flat[dec->bits >> (dec->nbits - dec->tab.flatbits)];
	if(c == ~0)
		c = hdecsym(dec, &dec->tab);
	else{
		dec->nbits -= c & 0xff;
		c >>= 8;
	}

	/*
	 * reverse funny run-length coding
	 */
	if(c < ZBase){
		dec->nzero = dec->base << c;
		dec->base <<= 1;
		dec->nzero--;
		return 0;
	}

	dec->base = 1;
	c -= LitBase;
	return c;
}

static void
hufftab(Decode *dec, Huff *h, uchar *hb, ulong *bitcount, int maxleaf, int maxbits, int flatbits)
{
	ulong c, code, nc[MaxHuffBits+1];
	int i, b, ec;

	code = 0;
	c = 0;
	h->minbits = maxbits;
	for(b = 1; b <= maxbits; b++){
		h->last[b] = c;
		if(c == 0)
			h->minbits = b;
		c += bitcount[b];
		nc[b] = code << 1;
		code = (code << 1) + bitcount[b];
		if(code > (1 << b))
			fatal(dec, "corrupted huffman table");
		h->maxcode[b] = code - 1;
		h->last[b] += code - 1;
	}
	if(code != (1 << maxbits))
		fatal(dec, "huffman table not full");
	h->maxbits = b;
	if(flatbits > b)
		flatbits = b;
	h->flatbits = flatbits;

	b = 1 << flatbits;
	for(i = 0; i < b; i++)
		h->flat[i] = ~0;

	for(i = 0; i < maxleaf; i++){
		b = hb[i];
		if(b == 0)
			continue;
		c = nc[b]++;
		if(b <= flatbits){
			code = (i << 8) | b;
			ec = (c + 1) << (flatbits - b);
			if(ec > (1<<flatbits))
				fatal(dec, "flat code too big");
			for(c <<= (flatbits - b); c < ec; c++)
				h->flat[c] = code;
		}else{
			c = h->last[b] - c;
			if(c >= maxleaf)
				fatal(dec, "corrupted huffman table");
			h->decode[c] = i;
		}
	}
}

static void
hbflush(Decode *dec)
{
	ulong bitcount[MaxHuffBits+1];
	uchar tmtf[MaxHuffBits+1], *hb;
	int i, b, m, maxbits;

	dec->base = 1;
	dec->ndec = 0;

	hb = malloc(MaxLeaf * sizeof *hb);
	if(waserror()) {
		free(hb);
		nexterror();
	}

	/*
	 * read the tables for the tables
	 */
	for(i = 0; i <= MaxHuffBits; i++)
		bitcount[i] = 0;
	maxbits = 0;
	for(i = 0; i <= MaxHuffBits; i++){
		b = bitget(dec, 4);
		hb[i] = b;
		bitcount[b]++;
		if(b > maxbits)
			maxbits = b;
	}
	hufftab(dec, &dec->tab, hb, bitcount, MaxHuffBits+1, maxbits, MaxFlatbits);
	for(i = 0; i <= MaxHuffBits; i++){
		tmtf[i] = i;
		bitcount[i] = 0;
	}
	maxbits = 0;
	for(i = 0; i < dec->maxblocksym; i++){
		if(dec->nbits <= dec->tab.maxbits)
			fillbits(dec);
		dec->bits &= (1 << dec->nbits) - 1;
		m = dec->tab.flat[dec->bits >> (dec->nbits - dec->tab.flatbits)];
		if(m == ~0)
			m = hdecsym(dec, &dec->tab);
		else{
			dec->nbits -= m & 0xff;
			m >>= 8;
		}
		b = tmtf[m];
		for(; m > 0; m--)
			tmtf[m] = tmtf[m-1];
		tmtf[0] = b;

		if(b > MaxHuffBits)
			fatal(dec, "bit length too big");
		hb[i] = b;
		bitcount[b]++;
		if(b > maxbits)
			maxbits = b;
	}
	for(; i < MaxLeaf; i++)
		hb[i] = 0;

	hufftab(dec, &dec->tab, hb, bitcount, MaxLeaf, maxbits, MaxFlatbits);

	poperror();
	free(hb);
}

static void
fatal(Decode *dec, char *msg)
{
	USED(dec);
print("unsac: %s\n", msg);
	error(msg);
}
