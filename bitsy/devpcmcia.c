#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"

static PCMslot	slot[2];
int nslot = 2;

struct {
	Ref;
} pcmcia;

enum
{
	Qdir,
	Qmem,
	Qattr,
	Qctl,

	Nents = 3,
};

#define SLOTNO(c)	((c->qid.path>>8)&0xff)
#define TYPE(c)		(c->qid.path&0xff)
#define QID(s,t)	(((s)<<8)|(t))

static void increfp(PCMslot*);
static void decrefp(PCMslot*);
static void slotmap(int, ulong, ulong, ulong);
static void slottiming(int, int, int, int, int);

static int
pcmgen(Chan *c, Dirtab *, int , int i, Dir *dp)
{
	int slotno;
	Qid qid;
	long len;
	PCMslot *pp;
	char name[NAMELEN];

	if(i == DEVDOTDOT){
		devdir(c, (Qid){CHDIR, 0}, "#y", 0, eve, 0555, dp);
		return 1;
	}

	if(i >= Nents*nslot)
		return -1;

	slotno = i/Nents;
	pp = slot + slotno;
	len = 0;
	switch(i%Nents){
	case 0:
		qid.path = QID(slotno, Qmem);
		sprint(name, "pcm%dmem", slotno);
		len = pp->memlen;
		break;
	case 1:
		qid.path = QID(slotno, Qattr);
		sprint(name, "pcm%dattr", slotno);
		len = pp->memlen;
		break;
	case 2:
		qid.path = QID(slotno, Qctl);
		sprint(name, "pcm%dctl", slotno);
		break;
	}
	qid.vers = 0;
	devdir(c, qid, name, len, eve, 0660, dp);
	return 1;
}

/*
 *  set up the cards, default timing is 300 ns
 */
static void
pcmciareset(void)
{
	/* staticly map the whole area */
	slotmap(0, PHYSPCM0REGS, PYHSPCM0ATTR, PYHSPCM0MEM);
	slotmap(1, PHYSPCM1REGS, PYHSPCM1ATTR, PYHSPCM1MEM);

	/* set timing to the default, 300 */
	slottiming(0, 300, 300, 300, 0);
	slottiming(1, 300, 300, 300, 0);
}

static Chan*
pcmciaattach(char *spec)
{
	return devattach('y', spec);
}

static int
pcmciawalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, pcmgen);
}

static void
pcmciastat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, pcmgen);
}

static Chan*
pcmciaopen(Chan *c, int omode)
{
	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else
		increfp(slot + SLOTNO(c));
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
pcmciaclose(Chan *c)
{
	if(c->flag & COPEN)
		if(c->qid.path != CHDIR)
			decrefp(slot+SLOTNO(c));
}

/* a memmove using only bytes */
static void
memmoveb(uchar *to, uchar *from, int n)
{
	while(n-- > 0)
		*to++ = *from++;
}

/* a memmove using only shorts & bytes */
static void
memmoves(uchar *to, uchar *from, int n)
{
	ushort *t, *f;

	if((((ulong)to) & 1) || (((ulong)from) & 1) || (n & 1)){
		while(n-- > 0)
			*to++ = *from++;
	} else {
		n = n/2;
		t = (ushort*)to;
		f = (ushort*)from;
		while(n-- > 0)
			*t++ = *f++;
	}
}

static long
pcmread(void *a, long n, ulong off, uchar *start, ulong len)
{
	if(off > len)
		return 0;
	if(off + n > len)
		n = len - off;
	memmoveb(a, start+off, n);
	return n;
}

static long
pcmctlread(void *a, long n, ulong off, PCMslot *pp)
{
	char *p, *buf, *e;

	buf = p = malloc(READSTR);
	if(waserror()){
		free(buf);
		nexterror();
	}
	e = p + READSTR;

	buf[0] = 0;
	if(pp->occupied){
		p = seprint(p, e, "occupied\n");
		if(pp->verstr[0])
			p = seprint(p, e, "version %s\n", pp->verstr);
	}
	USED(p);

	n = readstr(off, a, n, buf);
	free(buf);
	poperror();
	return n;
}

static long
pcmciaread(Chan *c, void *a, long n, vlong off)
{
	PCMslot *pp;
	ulong offset = off;

	pp = slot + SLOTNO(c);

	switch(TYPE(c)){
	case Qdir:
		return devdirread(c, a, n, 0, 0, pcmgen);
	case Qmem:
		if(!pp->occupied)
			error(Eio);
		return pcmread(a, n, offset, pp->mem, 64*OneMeg);
	case Qattr:
		if(!pp->occupied)
			error(Eio);
		return pcmread(a, n, offset, pp->attr, OneMeg);
	case Qctl:
		return pcmctlread(a, n, offset, pp);
	}
	error(Ebadarg);
	return -1;	/* not reached */
}

static long
pcmwrite(void *a, long n, ulong off, uchar *start, ulong len)
{
	if(off > len)
		error(Eio);
	if(off + n > len)
		error(Eio);
	memmoveb(start+off, a, n);
	return n;
}

static long
pcmctlwrite(char *p, long n, ulong off, PCMslot *pp)
{
	/* nothing yet */
	USED(p, n, off, pp);
	error(Ebadarg);
	return 0;
}

static long
pcmciawrite(Chan *c, void *a, long n, vlong off)
{
	PCMslot *pp;
	ulong offset = off;

	pp = slot + SLOTNO(c);

	switch(TYPE(c)){
	case Qmem:
		if(!pp->occupied)
			error(Eio);
		return pcmwrite(a, n, offset, pp->mem, 64*OneMeg);
	case Qattr:
		if(!pp->occupied)
			error(Eio);
		return pcmwrite(a, n, offset, pp->attr, OneMeg);
	case Qctl:
		if(!pp->occupied)
			error(Eio);
		return pcmctlwrite(a, n, offset, pp);
	}
	error(Ebadarg);
	return -1;	/* not reached */
}

Dev pcmciadevtab = {
	'y',
	"pcmcia",

	pcmciareset,
	devinit,
	pcmciaattach,
	devclone,
	pcmciawalk,
	pcmciastat,
	pcmciaopen,
	devcreate,
	pcmciaclose,
	pcmciaread,
	devbread,
	pcmciawrite,
	devbwrite,
	devremove,
	devwstat,
};

/* see what's there */
static void
slotinfo(void)
{
	ulong x = gpioregs->level;

	if(x & GPIO_OPT_IND_i){
		/* no expansion pack */
		slot[0].occupied = 0;
		slot[1].occupied = 0;
	} else {
		slot[0].occupied = (x & GPIO_CARD_IND0_i) == 0;
		slot[1].occupied = (x & GPIO_CARD_IND1_i) == 0;
	}
}

/* use reference card to turn cards on and off */
static void
increfp(PCMslot *pp)
{
	if(incref(&pcmcia) == 1){
		egpiobits(EGPIO_exp_nvram_power|EGPIO_exp_full_power, 1);
		egpiobits(EGPIO_pcmcia_reset, 0);
		delay(100);	/* time to power up */
	}

	incref(pp);

	slotinfo();
}

static void
decrefp(PCMslot *pp)
{
	slotinfo();
	decref(pp);
	if(decref(&pcmcia) == 0)
		egpiobits(EGPIO_exp_nvram_power|EGPIO_exp_full_power, 0);
}

/*
 *  the regions are staticly mapped
 */
static void
slotmap(int slotno, ulong regs, ulong attr, ulong mem)
{
	PCMslot *pp;

	pp = &slot[slotno];
	pp->slotno = slotno;
	pp->memlen = 64*OneMeg;
	pp->msec = ~0;
	pp->verstr[0] = 0;

	pp->mem = mapmem(mem, 64*OneMeg, 0);
	pp->memmap.ca = 0;
	pp->memmap.cea = 64*MB;
	pp->memmap.isa = (ulong)pp->mem;
	pp->memmap.len = 64*OneMeg;
	pp->memmap.attr = 0;

	pp->attr = mapmem(attr, OneMeg, 0);
	pp->attrmap.ca = 0;
	pp->attrmap.cea = MB;
	pp->attrmap.isa = (ulong)pp->attr;
	pp->attrmap.len = OneMeg;
	pp->attrmap.attr = 1;

	pp->regs = mapspecial(regs, 32*1024);
}
PCMmap*
pcmmap(int slotno, ulong, int, int attr)
{
	if(slotno > nslot)
		panic("pcmmap");
	if(attr)
		return &slot[slotno].attrmap;
	else
		return &slot[slotno].memmap;
}
void
pcmunmap(int, PCMmap*)
{
}

/*
 *  setup card timings
 *    times are in ns
 *    count = ceiling[access-time/(2*3*T)] - 1, where T is a processor cycle
 *
 */
static int
ns2count(int ns)
{
	ulong y;

	/* get 100 times cycle time */
	y = 100000000/(conf.hz/1000);

	/* get 10 times ns/(cycle*6) */
	y = (1000*ns)/(6*y);

	/* round up */
	y += 9;
	y /= 10;

	/* subtract 1 */
	return y-1;
}
static void
slottiming(int slotno, int tio, int tattr, int tmem, int fast)
{
	ulong x;

	x = 0;
	if(fast)
		x |= 1<<MECR_fast0;
	x |= ns2count(tio) << MECR_io0;
	x |= ns2count(tattr) << MECR_attr0;
	x |= ns2count(tmem) << MECR_mem0;
	if(slotno == 0){
		x |= memconfregs->mecr & 0xffff0000;
	} else {
		x <<= 16;
		x |= memconfregs->mecr & 0xffff;
	}
	memconfregs->mecr = x;
}
