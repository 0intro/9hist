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
	MaxSelHuffBits	= 15,			/* max bits for a table selector huffman code */

	Nhuffblock	= 16*1024,		/* symbols encoded before output */
	Nhuffslop	= 64,			/* slop for stuffing zeros, etc. */
};

struct Huff
{
	int	minbits;
	int	maxbits;
	int	flatbits;
	ulong	flat[1<<8];
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

	jmp_buf	errjmp;

	uchar	*src;				/* input buffer */
	uchar	*smax;				/* limit */
};

static	void	fatal(Decode *dec, char*, ...);

static	int	hdec(Decode*);
static	void	hflush(Decode*);
static	void	hbflush(Decode*);
static	ulong	bitget(Decode*, int);
static	int	mtf(uchar*, int);

int
unsac(uchar *dst, uchar *src, int n, int nsrc)
{
	Decode dec;
	uchar *buf, front[256];
	ulong *suflink, sums[256];
	ulong sum;
	int i, m, I, j, c;

	buf = malloc(n+2);
	suflink = malloc((n+2) * sizeof *suflink);

	if(waserror()) {
		free(buf);
		free(suflink);
		nexterror();
	}

	dec.src = src;
	dec.smax = src + nsrc;

	dec.nbits = 0;
	dec.bits = 0;
	dec.nzero = 0;
	for(i = 0; i < 256; i++)
		front[i] = i;

	n++;
	I = bitget(&dec, 16);
	if(I >= n)
		fatal(&dec, "corrupted input file: n=%d I=%d", n, I);

	/*
	 * decode the character usage map
	 */
	for(i = 0; i < 256; i++)
		sums[i] = 0;
	c = bitget(&dec, 1);
	for(i = 0; i < 256; ){
		m = bitget(&dec, 8) + 1;
		while(m--){
			if(i >= 256)
				fatal(&dec, "bad format encoding char map %d", m);
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
	dec.maxblocksym = c + LitBase;

	/*
	 * huffman decoding, move to front decoding,
	 * along with character counting
	 */
	hbflush(&dec);
	for(i = 0; i < n; i++){
		if(i == I)
			continue;
		m = hdec(&dec);

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
	free(buf);
	free(suflink);
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
			fatal(dec, "premature eof 2: nbits %d", dec->nbits);
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
		fatal(dec, "too many bits consumed: b=%d minbits=%d maxbits=%d", b, h->minbits, h->maxbits);
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
hbflush(Decode *dec)
{
	dec->base = 1;
	dec->ndec = 0;
	hflush(dec);
}

static void
hufftab(Decode *dec, Huff *h, ulong *hb, ulong *bitcount, int maxleaf, int maxbits, int flatbits)
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
		fatal(dec, "huffman table not full %d %d", code, 1<<maxbits);
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
if(c > (1<<(b+1)))fatal(dec, "xx1");
			code = (i << 8) | b;
			ec = (c + 1) << (flatbits - b);
			if(ec > (1<<flatbits))
				fatal(dec, "too big: ec=%d c=%d %d b=%d nc=%d\n", ec, c, c & ((1<<b)-1), b, nc[b]);
			for(c <<= (flatbits - b); c < ec; c++)
				h->flat[c] = code;
		}else{
			c = h->last[b] - c;
			if(c >= maxleaf)
				fatal(dec, "corrupted huffman table: c=%d maxleaf=%d i=%d b=%d maxbits=%d last=%d, nc=%d",
					c, maxleaf, i, b, maxbits, h->last[b], nc[b]);
			h->decode[c] = i;
		}
	}
}

static void
hflush(Decode *dec)
{
	Huff codetab;
	ulong bitcount[MaxHuffBits+1], hb[MaxLeaf];
	uchar tmtf[MaxHuffBits+1];
	int i, b, m, maxbits;

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
	hufftab(dec, &codetab, hb, bitcount, MaxHuffBits+1, maxbits, 8);
	for(i = 0; i <= MaxHuffBits; i++)
		tmtf[i] = i;
	for(i = 0; i <= MaxHuffBits; i++)
		bitcount[i] = 0;
	maxbits = 0;
	for(i = 0; i < dec->maxblocksym; i++){
		if(dec->nbits <= codetab.maxbits)
			fillbits(dec);
		dec->bits &= (1 << dec->nbits) - 1;
		m = codetab.flat[dec->bits >> (dec->nbits - codetab.flatbits)];
		if(m == ~0)
			m = hdecsym(dec, &codetab);
		else{
			dec->nbits -= m & 0xff;
			m >>= 8;
		}
		b = tmtf[m];
		for(; m > 0; m--)
			tmtf[m] = tmtf[m-1];
		tmtf[0] = b;

		if(b > MaxHuffBits)
			fatal(dec, "bit length %d too large, max %d i %d maxval %d", b, MaxHuffBits, i, dec->maxblocksym);
		hb[i] = b;
		bitcount[b]++;
		if(b > maxbits)
			maxbits = b;
	}
	for(; i < MaxLeaf; i++)
		hb[i] = 0;

	hufftab(dec, &dec->tab, hb, bitcount, MaxLeaf, maxbits, 8);
}

static void
fatal(Decode *dec, char *fmt, ...)
{
	char buf[128];
	va_list arg;

	va_start(arg, fmt);
	doprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);

	print("devsac: %s\n", buf);
	error(buf);
}
