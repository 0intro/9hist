#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "thwack.h"

typedef struct Huff	Huff;

enum
{
	StatBytes,
	StatOutBytes,
	StatLits,
	StatMatches,
	StatLitBits,
	StatOffBits,
	StatLenBits,

	StatProbe,
	StatProbeMiss,

	MaxStat
};

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
	int then, toff, w, ok;
	uchar *s, *t;

	s = *ss;
	if(esrc < s + MinMatch)
		return 0;

	toff = 0;
	for(; b < eblocks; b++){
		then = b->hash[(h ^ b->seq) & HashMask];
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
		ok = b->edata - t;
		if(esrc - s > ok)
			esrc = s + ok;

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
/*
#define hashit(c)	(((ulong)(c) * 0x6b43a9) >> (24 - HashLog))
*/
#define hashit(c)	((((ulong)(c) & 0xffffff) * 0x6b43a9b5) >> (32 - HashLog))

/*
 * lz77 compression with single lookup in a hash table for each block
 */
int
thwack(Thwack *tw, uchar *dst, uchar *src, int n, ulong seq, ulong stats[ThwStats])
{
	ThwBlock *eblocks, *b, blocks[CompBlocks];
	uchar *s, *ss, *sss, *esrc, *half, *twdst, *twdmax;
	ulong cont, cseq, bseq, cmask, code, twbits;
	int now, toff, lithist, h, len, slot, bits, use, twnbits, lits, matches, offbits, lenbits;

	if(n > ThwMaxBlock || n < MinMatch)
		return -1;

	twdst = dst;
	twdmax = dst + n;

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
	*twdst++ = seq - cseq;
	*twdst++ = cmask;

	cont = (s[0] << 16) | (s[1] << 8) | s[2];

	esrc = s + n;
	half = s + (n >> 1);
	twnbits = 0;
	twbits = 0;
	lits = 0;
	matches = 0;
	offbits = 0;
	lenbits = 0;
	lithist = ~0;
	while(s < esrc){
		h = hashit(cont);

		sss = s;
		toff = thwmatch(blocks, eblocks, &sss, esrc, h);
		ss = sss;

		len = ss - s;
		for(; twnbits >= 8; twnbits -= 8){
			if(twdst >= twdmax)
				return -1;
			*twdst++ = twbits >> (twnbits - 8);
		}
		if(len < MinMatch){
			toff = *s;
			lithist = (lithist << 1) | toff < 32 | toff > 127;
			if(lithist & 0x1e){
				twbits = (twbits << 9) | toff;
				twnbits += 9;
			}else if(lithist & 1){
				toff = (toff + 64) & 0xff;
				if(toff < 96){
					twbits = (twbits << 10) | toff;
					twnbits += 10;
				}else{
					twbits = (twbits << 11) | toff;
					twnbits += 11;
				}
			}else{
				twbits = (twbits << 8) | toff;
				twnbits += 8;
			}
			lits++;
			blocks->maxoff++;

			/*
			 * speed hack
			 * check for compression progress, bail if none achieved
			 */
			if(s > half){
				if(4 * blocks->maxoff < 5 * lits)
					return -1;
				half = esrc;
			}

			if(s + MinMatch <= esrc){
				blocks->hash[(h ^ blocks->seq) & HashMask] = now;
				if(s + MinMatch < esrc)
					cont = (cont << 8) | s[MinMatch];
			}
			now++;
			s++;
			continue;
		}

		blocks->maxoff += len;
		matches++;

		toff--;
		for(bits = OffBase; toff >= (1 << bits); bits++)
			;
		if(bits >= MaxOff+OffBase)
			panic("thwack offset");
		twbits = (twbits << 4) | 0x8 | (bits - OffBase);
		if(bits != OffBase)
			bits--;
		twbits = (twbits << bits) | toff & ((1 << bits) - 1);
		twnbits += bits + 4;
		offbits += bits + 4;

		len -= MinMatch;
		if(len < MaxFastLen){
			bits = lentab[len].bits;
			twbits = (twbits << bits) | lentab[len].encode;
			twnbits += bits;
			lenbits += bits;
		}else{
			for(; twnbits >= 8; twnbits -= 8){
				if(twdst >= twdmax)
					return -1;
				*twdst++ = twbits >> (twnbits - 8);
			}
			code = BigLenCode;
			bits = BigLenBits;
			use = BigLenBase;
			len -= MaxFastLen;
			while(len >= use){
				len -= use;
				code = (code + use) << 1;
				use <<= bits & 1;
				bits++;
			}
			if(bits > MaxLenDecode + BigLenBits)
				panic("length too big");
			twbits = (twbits << bits) | (code + len);
			twnbits += bits;
			lenbits += bits;
		}

		for(; s != ss; s++){
			if(s + MinMatch <= esrc){
				h = hashit(cont);
				blocks->hash[(h ^ blocks->seq) & HashMask] = now;
				if(s + MinMatch < esrc)
					cont = (cont << 8) | s[MinMatch];
			}
			now++;
		}
	}

	stats[StatBytes] += blocks->maxoff;
	stats[StatLits] += lits;
	stats[StatMatches] += matches;
	stats[StatLitBits] += (twdst - (dst + 2)) * 8 + twnbits - offbits - lenbits;
	stats[StatOffBits] += offbits;
	stats[StatLenBits] += lenbits;

	if(twnbits & 7){
		twbits <<= 8 - (twnbits & 7);
		twnbits += 8 - (twnbits & 7);
	}
	for(; twnbits >= 8; twnbits -= 8){
		if(twdst >= twdmax)
			return -1;
		*twdst++ = twbits >> (twnbits - 8);
	}

	tw->slot++;
	if(tw->slot >= EWinBlocks)
		tw->slot = 0;

	stats[StatOutBytes] += twdst - dst;

	return twdst - dst;
}
