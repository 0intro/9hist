#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "thwack.h"

typedef struct Huff	Huff;
struct Huff
{
	short	bits;				/* length of the code */
	ulong	encode;				/* the code */
};

static	Huff	lentab[MaxFastLen] =
{
	{1,	0x0},		/* 0 */
	{2,	0x2},		/* 10 */
	{4,	0xc},		/* 1100 */
	{4,	0xd},		/* 1101 */
	{5,	0x1c},		/* 11100 */
	{6,	0x3a},		/* 111010 */
	{6,	0x3b},		/* 111011 */
	{7,	0x78},		/* 1111000 */
	{7,	0x79},		/* 1111001 */
};

static	void	bitput(Thwack *tw, int c, int n);
static	int	iomegaput(Thwack *tw, ulong v);

void
thwackinit(Thwack *tw)
{
	int i;

	memset(tw, 0, sizeof *tw);
	for(i = 0; i < EWinBlocks; i++){
		tw->blocks[i].data = tw->data[i];
		tw->blocks[i].edata = tw->blocks[i].data;
		tw->blocks[i].hash = tw->hash[i];
		tw->blocks[i].acked = 0;
	}
}

/*
 * acknowledgement for block seq & nearby preds
 */
void
thwackack(Thwack *tw, ulong seq, ulong mask)
{
	int slot, b;

	slot = tw->slot;
	for(;;){
		for(;;){
			slot--;
			if(slot < 0)
				slot += EWinBlocks;
			if(slot == tw->slot)
				return;
			if(tw->blocks[slot].seq == seq){
				tw->blocks[slot].acked = 1;
				break;
			}
		}

		if(mask == 0)
			break;
		do{
			b = mask & 1;
			seq--;
			mask >>= 1;
		}while(!b);
	}
}

/*
 * find a string in the dictionary
 */
static int
thwmatch(ThwBlock *b, ThwBlock *eblocks, uchar **ss, uchar *esrc, ulong h)
{
	int then, toff, w;
	uchar *s, *t;

	s = *ss;
	if(esrc < s + MinMatch)
		return 0;

	toff = 0;
	for(; b < eblocks; b++){
		then = b->hash[h];
		toff += b->maxoff;
		w = (ushort)(then - b->begin);

		if(w >= b->maxoff)
			continue;

		/*
		 * don't need to check for the end because
		 * 1) s too close check above
		 * 2) entries too close not added to hash tables
		 */
		t = w + b->data;
		if(s[0] != t[0] || s[1] != t[1] || s[2] != t[2])
			continue;
		if(esrc - s > b->edata - t)
			esrc = s + (b->edata - t);

		t += 3;
		for(s += 3; s < esrc; s++){
			if(*s != *t)
				break;
			t++;
		}
		*ss = s;
		return toff - w;
	}
	return 0;
}

/*
 * knuth vol. 3 multiplicative hashing
 * each byte x chosen according to rules
 * 1/4 < x < 3/10, 1/3 x < < 3/7, 4/7 < x < 2/3, 7/10 < x < 3/4
 * with reasonable spread between the bytes & their complements
 *
 * the 3 byte value appears to be as almost good as the 4 byte value,
 * and might be faster on some machines
 */
//#define hashit(c)	(((((ulong)(c) & 0xffffff) * 0x6b43a9b5) >> (32 - HashLog)) & HashMask)
#define hashit(c)	((((ulong)(c) * 0x6b43a9) >> (24 - HashLog)) & HashMask)

/*
 * lz77 compression with single lookup in a hash table for each block
 */
int
thwack(Thwack *tw, uchar *dst, uchar *src, int n, ulong seq)
{
	ThwBlock *eblocks, *b, blocks[CompBlocks];
	uchar *s, *ss, *sss, *esrc, *half;
	ulong cont, cseq, bseq, cmask, code;
	int now, toff;
	int h, m, slot, bits, use, totmatched;

	if(n > ThwMaxBlock || n < MinMatch || waserror())
		return -1;

	tw->dst = dst;
	tw->dmax = dst + n;
	tw->nbits = 0;

	/*
	 * add source to the coding window
	 * there is always enough space
	 */
	slot = tw->slot;
	b = &tw->blocks[slot];
	b->seq = seq;
	b->acked = 0;
	now = b->begin + b->maxoff;
	s = b->data;
	memmove(s, src, n);
	b->edata = s + n;
	b->begin = now;
	b->maxoff = n;

	/*
	 * set up the history blocks
	 */
	cseq = seq;
	cmask = 0;
	*blocks = *b;
	b = blocks;
	b->maxoff = 0;
	b++;
	while(b < blocks + CompBlocks){
		slot--;
		if(slot < 0)
			slot += EWinBlocks;
		if(slot == tw->slot)
			break;
		if(!tw->blocks[slot].acked)
			continue;
		bseq = tw->blocks[slot].seq;
		if(cseq == seq){
			if(seq - bseq >= MaxSeqStart)
				break;
			cseq = bseq;
		}else if(cseq - bseq > MaxSeqMask)
			break;
		else
			cmask |= 1 << (cseq - bseq - 1);
		*b = tw->blocks[slot];
		b++;
	}
	eblocks = b;
	bitput(tw, ((seq - cseq) << MaxSeqMask) | cmask, 16);

	cont = (s[0] << 16) | (s[2] << 8) | s[2];

	totmatched = 0;
	esrc = s + n;
	half = s + (n >> 1);
	while(s < esrc){
		h = hashit(cont);

		sss = s;
		toff = thwmatch(blocks, eblocks, &sss, esrc, h);
		ss = sss;

		m = ss - s;
		if(m < MinMatch){
			bitput(tw, 0x100|*s, 9);
			ss = s + 1;
		}else{
			totmatched += m;

			toff--;
			for(bits = OffBase; toff >= (1 << bits); bits++)
				;
			if(bits >= MaxOff+OffBase)
				error("thwack offset");
			bitput(tw, bits - OffBase, 4);
			if(bits != OffBase)
				bits--;
			bitput(tw, toff & ((1 << bits) - 1), bits);

			m -= MinMatch;
			if(m < MaxFastLen){
				bitput(tw, lentab[m].encode, lentab[m].bits);
			}else{
				code = BigLenCode;
				bits = BigLenBits;
				use = BigLenBase;
				m -= MaxFastLen;
				while(m >= use){
					m -= use;
					code = (code + use) << 1;
					use <<= bits & 1;
					bits++;
				}
				bitput(tw, (code + m), bits);
			}
		}
		blocks->maxoff += ss - s;

		/*
		 * speed hack
		 * check for compression progress, bail if none achieved
		 */
		if(s < half && ss >= half && totmatched * 10 < n)
			error("thwack likely expanding");

		for(; s != ss; s++){
			if(s + MinMatch <= esrc){
				h = hashit(cont);
				blocks->hash[h] = now;
				if(s + MinMatch < esrc)
					cont = (cont << 8) | s[MinMatch];
			}
			now++;
		}
	}

	if(tw->nbits)
		bitput(tw, 0, 8 - tw->nbits);
	tw->slot++;
	if(tw->slot >= EWinBlocks)
		tw->slot = 0;

	poperror();
	return tw->dst - dst;
}

static void
bitput(Thwack *tw, int c, int n)
{
	tw->bits = (tw->bits << n) | c;
	for(tw->nbits += n; tw->nbits >= 8; tw->nbits -= 8){
		if(tw->dst >= tw->dmax)
			error("thwack expanding");
		*tw->dst++ = tw->bits >> (tw->nbits - 8);
	}
}
