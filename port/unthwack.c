#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "thwack.h"

typedef struct HuffDec		HuffDec;

struct HuffDec
{
	ulong	maxcode[MaxFastLen];
	ulong	last[MaxFastLen];
	ulong	decode[MaxFastLen];
};

static HuffDec lentab = 
{
/*	0	1	2	3	4	5	6	7	*/
	{0,	0,	0x2,	0,	0xd,	0x1c,	0x3b,	0x79},
	{-1,	0+0,	0x2+1,	-1,	0xd+2,	0x1c+4,	0x3b+5,	0x79+7},
	{
		0,
		1,
		3, 2,
		4,
		6, 5,
		8, 7,
	},
};

void
unthwackinit(Unthwack *ut)
{
	int i;

	memset(ut, 0, sizeof *ut);
	for(i = 0; i < EWinBlocks; i++)
		ut->blocks[i].data = ut->data[i];
}

int
unthwack(Unthwack *ut, uchar *dst, int ndst, uchar *src, int nsrc, ulong seq)
{
	UnthwBlock blocks[CompBlocks], *b, *eblocks;
	uchar *s, *es, *d, *dmax, *smax, lit;
	ulong cmask, cseq, bseq, utbits, lithist;
	int off, len, bits, slot, tslot, use, code, utnbits, overbits;

	if(nsrc < 4 || nsrc > ThwMaxBlock)
		return -1;

	/*
	 * find the correct slot for this block,
	 * the oldest block around.  the encoder
	 * doesn't use a history at wraparound,
	 * so don't worry about that case.
	 */
	tslot = ut->slot;
	for(;;){
		slot = tslot - 1;
		if(slot < 0)
			slot += DWinBlocks;
		if(ut->blocks[slot].seq <= seq)
			break;
		ut->blocks[slot] = ut->blocks[slot];
		tslot = slot;
	}
	b = blocks;
	ut->blocks[tslot].seq = seq;
	ut->blocks[tslot].maxoff = 0;
	*b = ut->blocks[tslot];
	d = b->data;
	dmax = d + ndst;

	/*
	 * set up the history blocks
	 */
	cseq = seq - src[0];
	cmask = src[1];
	b++;
	slot = tslot;
	while(cseq != seq && b < blocks + CompBlocks){
		slot--;
		if(slot < 0)
			slot += DWinBlocks;
		if(slot == ut->slot)
			break;
		bseq = ut->blocks[slot].seq;
		if(bseq == cseq){
			*b = ut->blocks[slot];
			b++;
			if(cmask == 0){
				cseq = seq;
				break;
			}
			do{
				bits = cmask & 1;
				cseq--;
				cmask >>= 1;
			}while(!bits);
		}
	}
	eblocks = b;
	if(cseq != seq){
		print("blocks not in decompression window: cseq=%ld seq=%ld cmask=%lux nb=%ld\n", cseq, seq, cmask, eblocks - blocks);
		return -1;
	}

	smax = src + nsrc;
	src += 2;
	utnbits = 0;
	utbits = 0;
	overbits = 0;
	lithist = ~0;
	while(src < smax || utnbits - overbits >= MinDecode){
		while(utnbits <= 24){
			utbits <<= 8;
			if(src < smax)
				utbits |= *src++;
			else
				overbits += 8;
			utnbits += 8;
		}

		/*
		 * literal
		 */
		if(((utbits >> (utnbits - 1)) & 1) == 0){
			if(lithist & 0xf){
				utnbits -= 9;
				lit = (utbits >> utnbits) & 0xff;
				lit &= 255;
			}else{
				utnbits -= 8;
				lit = (utbits >> utnbits) & 0x7f;
				if(lit < 32){
					if(lit < 24){
						utnbits -= 2;
						lit = (lit << 2) | ((utbits >> utnbits) & 3);
					}else{
						utnbits -= 3;
						lit = (lit << 3) | ((utbits >> utnbits) & 7);
					}
					lit = (lit - 64) & 0xff;
				}
			}
			*d++ = lit;
			lithist = (lithist << 1) | lit < 32 | lit > 127;
			blocks->maxoff++;
			continue;
		}

		/*
		 * match; next 3 bits decode offset range
		 */
		utnbits -= 4;
		bits = (utbits >> utnbits) & ((1 << 3) - 1);
		if(bits){
			bits += OffBase - 1;
			off = 1 << bits;
		}else{
			bits = OffBase;
			off = 0;
		}
		utnbits -= bits;
		off |= (utbits >> utnbits) & ((1 << bits) - 1);
		off++;

		bits = 0;
		utbits &= (1 << utnbits) - 1;
		do{
			bits++;
			len = utbits >> (utnbits - bits);
		}while(bits < BigLenBits && len > lentab.maxcode[bits]);
		utnbits -= bits;

		if(bits < BigLenBits)
			len = lentab.decode[lentab.last[bits] - len];
		else{
			while(utnbits < MaxLenDecode){
				utbits <<= 8;
				if(src < smax)
					utbits |= *src++;
				else
					overbits += 8;
				utnbits += 8;
			}

			code = len - BigLenCode;
			len = MaxFastLen;
			bits = 8;
			use = BigLenBase;
			while(code >= use){
				len += use;
				code -= use;
				code <<= 1;
				utnbits--;
				code |= (utbits >> utnbits) & 1;
				use <<= bits & 1;
				bits++;
			}
			len += code;
		}

		len += MinMatch;

		b = blocks;
		while(off > b->maxoff){
			off -= b->maxoff;
			b++;
			if(b >= eblocks)
				return -1;
		}
		if(d + len > dmax
		|| b != blocks && len > off)
			return -1;
		s = b->data + b->maxoff - off;
		es = s + len;
		while(s < es)
			*d++ = *s++;
		blocks->maxoff += len;
	}
	if(utnbits < overbits)
		return -1;

	len = d - blocks->data;
	memmove(dst, blocks->data, len);
	ut->blocks[tslot].maxoff = len;

	ut->slot++;
	if(ut->slot >= DWinBlocks)
		ut->slot = 0;

	return len;
}
