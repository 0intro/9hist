#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "thwack.h"

typedef struct HuffDec		HuffDec;

struct HuffDec
{
	ulong	maxcode[MaxLen];
	ulong	last[MaxLen];
	ulong	decode[MaxLen];
};

static HuffDec lentab = 
{
/*	0	1	2	3	4	5	6	*/
	{0,	0,	0x2,	0,	0xe,	0x1e,	0x3f},
	{-1,	0+0,	0x2+1,	-1,	0xe+2,	0x1e+5,	0x3f+6},
	{
		0,
		1,
		7, 3, 2,
		4,
		6, 5
	},
};

static ulong	bitget(Unthwack *ut, int nb);
static ulong	iomegaget(Unthwack *ut);

void
unthwackinit(Unthwack *ut)
{
	int i;

	memset(ut, 0, sizeof *ut);
	for(i = 0; i < EWinBlocks; i++)
		ut->blocks[i].data = ut->data[i];
}

/*
 * to speed up, inline bitget
 */
int
unthwack(Unthwack *ut, uchar *dst, int ndst, uchar *src, int nsrc, ulong seq)
{
	UnthwBlock blocks[CompBlocks], *b, *eblocks;
	uchar *s, *es, *d, *dmax;
	ulong cmask, cseq, bseq, utbits, utnbits;
	int off, len, bits, slot, tslot;

	if(nsrc < 4 || nsrc > ThwMaxBlock || waserror())
		return -1;

	ut->src = src + 2;
	ut->smax = src + nsrc;

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
		print("blocks not in decompression window: cseq=%d seq=%d cmask=%ux nb=%d\n", cseq, seq, cmask, eblocks - blocks);
		error("unthwack bad window");
	}

	utnbits = 0;
	utbits = 0;
	while(ut->src < ut->smax || ut->nbits >= MinDecode){
		while(utnbits < 9){
			if(ut->src >= ut->smax)
				error("unthwack eof");
			utbits <<= 8;
			utbits |= *ut->src++;
			utnbits += 8;
		}
		utnbits -= 9;
		off = (utbits >> utnbits) & ((1 << 9) - 1);

		bits = off >> 5;
		if(bits >= MaxOff){
			*d++ = off;
			blocks->maxoff++;
			continue;
		}
		off &= (1 << 5) - 1;
		if(bits){
			bits--;
			off |= 1 << 5;
		}
		bits += OffBase - 5;
		off <<= bits;

		while(utnbits < bits){
			if(ut->src >= ut->smax)
				error("unthwack eof");
			utbits <<= 8;
			utbits |= *ut->src++;
			utnbits += 8;
		}
		utnbits -= bits;
		off |= (utbits >> utnbits) & ((1 << bits) - 1);
		off++;

		len = 0;
		bits = 0;
		do{
			len <<= 1;
			if(utnbits < 1){
				if(ut->src >= ut->smax)
					error("unthwack eof");
				utbits <<= 8;
				utbits |= *ut->src++;
				utnbits += 8;
			}
			utnbits--;
			len |= (utbits >> utnbits) & 1;
			bits++;
		}while(len > lentab.maxcode[bits]);
		len = lentab.decode[lentab.last[bits] - len];

		if(len == MaxLen - 1){
			ut->nbits = utnbits;
			ut->bits = utbits;
			len += iomegaget(ut) - 1;
			utnbits = ut->nbits;
			utbits = ut->bits;
		}
		len += MinMatch;

		b = blocks;
		while(off > b->maxoff){
			off -= b->maxoff;
			b++;
			if(b >= eblocks)
				error("unthwack offset");
		}
		if(d + len > dmax
		|| b != blocks && len > off)
			error("unthwack len");
		s = b->data + b->maxoff - off;
		es = s + len;
		while(s < es)
			*d++ = *s++;
		blocks->maxoff += len;
	}

	len = d - blocks->data;
	memmove(dst, blocks->data, len);
	ut->blocks[tslot].maxoff = len;

	ut->slot++;
	if(ut->slot >= DWinBlocks)
		ut->slot = 0;

	poperror();
	return len;
}

/*
 * elias's omega code, modified
 * for at least 3 bit transmission
 */
static ulong
iomegaget(Unthwack *ut)
{
	ulong v;
	int b;

	v = bitget(ut, 3);
	if((v & 0x4) == 0)
		return v + 1;
	for(;;){
		b = bitget(ut, 1);
		if(b == 0)
			return v + 1;
		if(v > 16)
			break;
		v--;
		v = (b << v) | bitget(ut, v);
	}
	error("unthwack iomegaget");
	return ~0;
}

static ulong
bitget(Unthwack *ut, int nb)
{
	int c;

	while(ut->nbits < nb){
		if(ut->src >= ut->smax)
			error("unthwack eof");
		c = *ut->src++;
		ut->bits <<= 8;
		ut->bits |= c;
		ut->nbits += 8;
	}
	ut->nbits -= nb;
	return (ut->bits >> ut->nbits) & ((1 << nb) - 1);
}
