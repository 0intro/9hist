#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

enum {
	BDSIZE=	1024,	/* IO memory reserved for buffer descriptors */
	CPMSIZE=	1024,	/* IO memory reserved for other uses */
};

static	Map	bdmapv[BDSIZE/sizeof(BD)];
static	RMap	bdmap = {"buffer descriptors"};

static	Map	cpmmapv[CPMSIZE/sizeof(ulong)];
static	RMap	cpmmap = {"CPM memory"};

static	Lock	cpmlock;

static struct {
	Lock;
	ulong	avail;
} brgens;

static struct {
	Lock;
	ulong	clash;	/* disable bits for mutually-exclusive options */
	ulong	active;	/* disable bit for current owner, or 0 */
	void	(*stop)(void*);
	void*	arg;
} scc2owner;

static	void	i2cspireloc(void);

/*
 * initialise the communications processor module
 * and associated device registers
 */
void
cpminit(void)
{
	IMM *io;

	io = m->iomem;
	io->rccr = 0;
	io->rmds = 0;
//	io->lccr &= ~1;	/* disable LCD */
	io->sdcr = 1;
	io->pcint = 0;	/* disable all port C interrupts */
	io->pcso = 0;
	io->pcdir =0;
	io->pcpar = 0;
	io->papar = 0;
	io->padir = 0;
	io->pbpar = 0;
	io->pbdir = 0;
	io->tgcr = 0x2222;	/* reset timers, low-power stop */
	eieio();

	for(io->cpcr = 0x8001; io->cpcr & 1;)	/* reset all CPM channels */
		eieio();

	mapinit(&bdmap, bdmapv, sizeof(bdmapv));
	mapfree(&bdmap, DPBASE, BDSIZE);
	mapinit(&cpmmap, cpmmapv, sizeof(cpmmapv));
	mapfree(&cpmmap, DPBASE+BDSIZE, CPMSIZE);

	if(m->cputype == 0x50 && (getimmr() & 0xFFFF) <= 0x2001)
		brgens.avail = 0x3;
	else
		brgens.avail = 0xF;

	scc2owner.clash = DisableEther|DisableIR|DisableRS232b;

	i2cspireloc();
}

/*
 * issue a request to a CPM device
 */
void
cpmop(int op, int cno, int param)
{
	IMM *io;

	ilock(&cpmlock);
	io = m->iomem;
	while(io->cpcr & 1)
		eieio();
	io->cpcr = (op<<8)|(cno<<4)|(param<<1)|1;
	eieio();
	while(io->cpcr & 1)
		eieio();
	iunlock(&cpmlock);
}

/*
 * lock the shared IO memory and return a reference to it
 */
IMM*
ioplock(void)
{
	ilock(&cpmlock);
	return m->iomem;
}

/*
 * release the lock on the shared IO memory
 */
void
iopunlock(void)
{
	eieio();
	iunlock(&cpmlock);
}

/*
 * connect SCCx clocks in NSMI mode (x=1 for USB)
 */
void
sccnmsi(int x, int rcs, int tcs)
{
	IMM *io;
	ulong v;
	int sh;

	sh = (x-1)*8;	/* each SCCx field in sicr is 8 bits */
	v = (((rcs&7)<<3) | (tcs&7)) << sh;
	io = ioplock();
	io->sicr = (io->sicr & ~(0xFF<<sh)) | v;
	iopunlock();
}

/*
 * request or grab (if force!=0) the use of SCC2
 * for the mode (uart, ether, etc) defined by the enable bits.
 * the `stop' function is invoked with the given argument
 * when the device is lost to another driver.
 */
int
scc2claim(int force, ulong enable, void (*stop)(void*), void *arg)
{
	ulong clash;

	clash = scc2owner.clash & ~enable;
	ilock(&scc2owner);
	if(/* ~m->bcsr[1] & clash */ 0){
		if(!force){
			iunlock(&scc2owner);
			return 0;
		}
		/* notify current owner */
		if(scc2owner.stop)
			scc2owner.stop(scc2owner.arg);
//		m->bcsr[1] |= clash;
	}
	scc2owner.stop = stop;
	scc2owner.arg = arg;
	scc2owner.active = enable;
//	m->bcsr[1] &= ~enable;	/* the bits are active low */
	iunlock(&scc2owner);
	/* beware: someone else can claim it during the `successful' return */
	return 1;
}

/*
 * if SCC2 is currently enabled, shut it down
 */
void
scc2stop(void *a)
{
	SCC *scc;

	scc = a;
	if(scc->gsmrl & (3<<4)){
		cpmop(GracefulStopTx, SCC2ID, 0);
		cpmop(CloseRxBD, SCC2ID, 0);
		delay(1);
		scc->gsmrl &= ~(3<<4);	/* disable current use */
//		m->bcsr[1] |= scc2owner.clash;
	}
}

/*
 * yield up control of SCC2
 */
void
scc2free(void)
{
	ilock(&scc2owner);
//	m->bcsr[1] |= scc2owner.active;
	scc2owner.active = 0;
	iunlock(&scc2owner);
}

/*
 * allocate a buffer descriptor
 */
BD *
bdalloc(int n)
{
	ulong a;

	a = rmapalloc(&bdmap, 0, n*sizeof(BD), sizeof(BD));
	if(a == 0)
		panic("bdalloc");
	return KADDR(a);
}

/*
 * free a buffer descriptor
 */
void
bdfree(BD *b, int n)
{
	if(b){
		eieio();
		mapfree(&bdmap, PADDR(b), n*sizeof(BD));
	}
}

/*
 * allocate memory from the shared IO memory space
 */
void *
cpmalloc(int n, int align)
{
	ulong a;

	a = rmapalloc(&cpmmap, 0, n, align);
	if(a == 0)
		panic("cpmalloc");
	return KADDR(a);
}

/*
 * free previously allocated shared memory
 */
void
cpmfree(void *p, int n)
{
	if(p != nil && n > 0){
		eieio();
		mapfree(&cpmmap, PADDR(p), n);
	}
}

/*
 * allocate a baud rate generator, returning its index
 * (or -1 if none is available)
 */
int
brgalloc(void)
{
	int n;

	lock(&brgens);
	for(n=0; brgens.avail!=0; n++)
		if(brgens.avail & (1<<n)){
			brgens.avail &= ~(1<<n);
			unlock(&brgens);
			return n;
		}
	unlock(&brgens);
	return -1;
}

/*
 * free a previously allocated baud rate generator
 */
void
brgfree(int n)
{
	if(n >= 0){
		if(n > 3 || brgens.avail & (1<<n))
			panic("brgfree");
		lock(&brgens);
		brgens.avail |= 1 << n;
		unlock(&brgens);
	}
}

/*
 * return a value suitable for loading into a baud rate
 * generator to produce the given rate if the generator
 * is prescaled by the given amount (typically 16).
 * the value must be or'd with BaudEnable to start the generator.
 */
ulong
baudgen(int rate, int scale)
{
	int d;

	rate *= scale;
	d = (2*m->cpuhz+rate)/(2*rate) - 1;
	if(d < 0)
		d = 0;
	if(d >= (1<<12))
		return ((d+15)>>(4-1))|1;	/* divider too big: enable prescale by 16 */
	return d<<1;
}

/*
 * initialise receive and transmit buffer rings.
 */
int
ioringinit(Ring* r, int nrdre, int ntdre, int bufsize)
{
	int i, x;

	/* the ring entries must be aligned on sizeof(BD) boundaries */
	r->nrdre = nrdre;
	if(r->rdr == nil)
		r->rdr = bdalloc(nrdre);
	if(r->rrb == nil)
		r->rrb = malloc(nrdre*bufsize);
	dcflush(r->rrb, nrdre*bufsize);
	if(r->rdr == nil || r->rrb == nil)
		return -1;
	x = PADDR(r->rrb);
	for(i = 0; i < nrdre; i++){
		r->rdr[i].length = 0;
		r->rdr[i].addr = x;
		r->rdr[i].status = BDEmpty|BDInt;
		x += bufsize;
	}
	r->rdr[i-1].status |= BDWrap;
	r->rdrx = 0;

	r->ntdre = ntdre;
	if(r->tdr == nil)
		r->tdr = bdalloc(ntdre);
	if(r->txb == nil)
		r->txb = malloc(ntdre*sizeof(Block*));
	if(r->tdr == nil || r->txb == nil)
		return -1;
	for(i = 0; i < ntdre; i++){
		r->txb[i] = nil;
		r->tdr[i].addr = 0;
		r->tdr[i].length = 0;
		r->tdr[i].status = 0;
	}
	r->tdr[i-1].status |= BDWrap;
	r->tdrh = 0;
	r->tdri = 0;
	r->ntq = 0;
	return 0;
}

/*
 * Allocate a new parameter block for I2C or SPI,
 * and plant a pointer to it for the microcode,
 * returning the kernel address.
 * See motorola errata and microcode package:
 * the design botch is that the parameters for the SCC2 ethernet overlap the
 * SPI/I2C parameter space; this compensates by relocating the latter.
 * This routine may be used iff i2cspireloc is used (and it is, above).
 */
void*
cpmparam(ulong olda, int nb)
{
	void *p;

	if(olda < INTMEM)
		olda += INTMEM;
	if(olda != SPIP && olda != I2CP)
		return KADDR(olda);
	p = cpmalloc(nb, 32);	/* ``RPBASE must be multiple of 32'' */
	if(p == nil)
		return p;
	*(ushort*)KADDR(olda+0x2C) = PADDR(p);	/* set RPBASE */
	eieio();
	return p;
}

/*
 * I2C/SPI microcode package from Motorola
 * (to relocate I2C/SPI parameters), which was distributed
 * on their web site in S-record format.
 *
 *	May 1998
 */

/*S00600004844521B*/
static	ulong	ubase1 = 0x2000;
static	ulong	ucode1[] = {
 /* #02202000 */ 0x7FFFEFD9,
 /* #02202004 */ 0x3FFD0000,
 /* #02202008 */ 0x7FFB49F7,
 /* #0220200C */ 0x7FF90000,
 /* #02202010 */ 0x5FEFADF7,
 /* #02202014 */ 0x5F89ADF7,
 /* #02202018 */ 0x5FEFAFF7,
 /* #0220201C */ 0x5F89AFF7,
 /* #02202020 */ 0x3A9CFBC8,
 /* #02202024 */ 0xE7C0EDF0,
 /* #02202028 */ 0x77C1E1BB,
 /* #0220202C */ 0xF4DC7F1D,
 /* #02202030 */ 0xABAD932F,
 /* #02202034 */ 0x4E08FDCF,
 /* #02202038 */ 0x6E0FAFF8,
 /* #0220203C */ 0x7CCF76CF,
 /* #02202040 */ 0xFD1FF9CF,
 /* #02202044 */ 0xABF88DC6,
 /* #02202048 */ 0xAB5679F7,
 /* #0220204C */ 0xB0937383,
 /* #02202050 */ 0xDFCE79F7,
 /* #02202054 */ 0xB091E6BB,
 /* #02202058 */ 0xE5BBE74F,
 /* #0220205C */ 0xB3FA6F0F,
 /* #02202060 */ 0x6FFB76CE,
 /* #02202064 */ 0xEE0DF9CF,
 /* #02202068 */ 0x2BFBEFEF,
 /* #0220206C */ 0xCFEEF9CF,
 /* #02202070 */ 0x76CEAD24,
 /* #02202074 */ 0x90B2DF9A,
 /* #02202078 */ 0x7FDDD0BF,
 /* #0220207C */ 0x4BF847FD,
 /* #02202080 */ 0x7CCF76CE,
 /* #02202084 */ 0xCFEF7E1F,
 /* #02202088 */ 0x7F1D7DFD,
 /* #0220208C */ 0xF0B6EF71,
 /* #02202090 */ 0x7FC177C1,
 /* #02202094 */ 0xFBC86079,
 /* #02202098 */ 0xE722FBC8,
 /* #0220209C */ 0x5FFFDFFF,
 /* #022020A0 */ 0x5FB2FFFB,
 /* #022020A4 */ 0xFBC8F3C8,
 /* #022020A8 */ 0x94A67F01,
 /* #022020AC */ 0x7F1D5F39,
 /* #022020B0 */ 0xAFE85F5E,
 /* #022020B4 */ 0xFFDFDF96,
 /* #022020B8 */ 0xCB9FAF7D,
 /* #022020BC */ 0x5FC1AFED,
 /* #022020C0 */ 0x8C1C5FC1,
 /* #022020C4 */ 0xAFDD5FC3,
 /* #022020C8 */ 0xDF9A7EFD,
 /* #022020CC */ 0xB0B25FB2,
 /* #022020D0 */ 0xFFFEABAD,
 /* #022020D4 */ 0x5FB2FFFE,
 /* #022020D8 */ 0x5FCE600B,
 /* #022020DC */ 0xE6BB600B,
 /* #022020E0 */ 0x5FCEDFC6,
 /* #022020E4 */ 0x27FBEFDF,
 /* #022020E8 */ 0x5FC8CFDE,
 /* #022020EC */ 0x3A9CE7C0,
 /* #022020F0 */ 0xEDF0F3C8,
 /* #022020F4 */ 0x7F0154CD,
 /* #022020F8 */ 0x7F1D2D3D,
 /* #022020FC */ 0x363A7570,
 /* #02202100 */ 0x7E0AF1CE,
 /* #02202104 */ 0x37EF2E68,
 /* #02202108 */ 0x7FEE10EC,
 /* #0220210C */ 0xADF8EFDE,
 /* #02202110 */ 0xCFEAE52F,
 /* #02202114 */ 0x7D0FE12B,
 /* #02202118 */ 0xF1CE5F65,
 /* #0220211C */ 0x7E0A4DF8,
 /* #02202120 */ 0xCFEA5F72,
 /* #02202124 */ 0x7D0BEFEE,
 /* #02202128 */ 0xCFEA5F74,
 /* #0220212C */ 0xE522EFDE,
 /* #02202130 */ 0x5F74CFDA,
 /* #02202134 */ 0x0B627385,
 /* #02202138 */ 0xDF627E0A,
 /* #0220213C */ 0x30D8145B,
 /* #02202140 */ 0xBFFFF3C8,
 /* #02202144 */ 0x5FFFDFFF,
 /* #02202148 */ 0xA7F85F5E,
 /* #0220214C */ 0xBFFE7F7D,
 /* #02202150 */ 0x10D31450,
 /* #02202154 */ 0x5F36BFFF,
 /* #02202158 */ 0xAF785F5E,
 /* #0220215C */ 0xBFFDA7F8,
 /* #02202160 */ 0x5F36BFFE,
 /* #02202164 */ 0x77FD30C0,
 /* #02202168 */ 0x4E08FDCF,
 /* #0220216C */ 0xE5FF6E0F,
 /* #02202170 */ 0xAFF87E1F,
 /* #02202174 */ 0x7E0FFD1F,
 /* #02202178 */ 0xF1CF5F1B,
 /* #0220217C */ 0xABF80D5E,
 /* #02202180 */ 0x5F5EFFEF,
 /* #02202184 */ 0x79F730A2,
 /* #02202188 */ 0xAFDD5F34,
 /* #0220218C */ 0x47F85F34,
 /* #02202190 */ 0xAFED7FDD,
 /* #02202194 */ 0x50B24978,
 /* #02202198 */ 0x47FD7F1D,
 /* #0220219C */ 0x7DFD70AD,
 /* #022021A0 */ 0xEF717EC1,
 /* #022021A4 */ 0x6BA47F01,
 /* #022021A8 */ 0x2D267EFD,
 /* #022021AC */ 0x30DE5F5E,
 /* #022021B0 */ 0xFFFD5F5E,
 /* #022021B4 */ 0xFFEF5F5E,
 /* #022021B8 */ 0xFFDF0CA0,
 /* #022021BC */ 0xAFED0A9E,
 /* #022021C0 */ 0xAFDD0C3A,
 /* #022021C4 */ 0x5F3AAFBD,
 /* #022021C8 */ 0x7FBDB082,
 /* #022021CC */ 0x5F8247F8,
};

/*S00600004844521B*/
static	ulong	ubase2 = 0x2F00;
static	ulong	ucode2[] = {
 /* #02202F00 */ 0x3E303430,
 /* #02202F04 */ 0x34343737,
 /* #02202F08 */ 0xABF7BF9B,
 /* #02202F0C */ 0x994B4FBD,
 /* #02202F10 */ 0xBD599493,
 /* #02202F14 */ 0x349FFF37,
 /* #02202F18 */ 0xFB9B177D,
 /* #02202F1C */ 0xD9936956,
 /* #02202F20 */ 0xBBFDD697,
 /* #02202F24 */ 0xBDD2FD11,
 /* #02202F28 */ 0x31DB9BB3,
 /* #02202F2C */ 0x63139637,
 /* #02202F30 */ 0x93733693,
 /* #02202F34 */ 0x193137F7,
 /* #02202F38 */ 0x331737AF,
 /* #02202F3C */ 0x7BB9B999,
 /* #02202F40 */ 0xBB197957,
 /* #02202F44 */ 0x7FDFD3D5,
 /* #02202F48 */ 0x73B773F7,
 /* #02202F4C */ 0x37933B99,
 /* #02202F50 */ 0x1D115316,
 /* #02202F54 */ 0x99315315,
 /* #02202F58 */ 0x31694BF4,
 /* #02202F5C */ 0xFBDBD359,
 /* #02202F60 */ 0x31497353,
 /* #02202F64 */ 0x76956D69,
 /* #02202F68 */ 0x7B9D9693,
 /* #02202F6C */ 0x13131979,
 /* #02202F70 */ 0x79376935,
};

/*
 * compensate for chip design botch by installing
 * microcode to relocate I2C and SPI parameters away
 * from the ethernet parameters
 */
static void
i2cspireloc(void)
{
	IMM *io;
	static int done;

	if(done)
		return;
	io = m->iomem;
	io->rccr &= ~3;
	memmove((uchar*)m->iomem+ubase1, ucode1, sizeof(ucode1));
	memmove((uchar*)m->iomem+ubase2, ucode2, sizeof(ucode2));
	io->rctr1 = 0x802a;	/* relocate SPI */
	io->rctr2 = 0x8028;	/* relocate SPI */
	io->rctr3 = 0x802e;	/* relocate I2C */
	io->rctr4 = 0x802c;	/* relocate I2C */
	io->rccr |= 1;
	done = 1;
}
