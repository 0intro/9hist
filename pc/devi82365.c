#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "devtab.h"

/*
 *  Support for up to 4 Slot card slots.  Generalizing above that is hard
 *  since addressing is not obvious. - presotto
 *
 *  WARNING: This has never been tried with more than one card slot.
 */

/*
 *  Intel 82365SL PCIC controller for the Slot or
 *  Cirrus Logic PD6710/PD6720 which is mostly register compatible
 */
enum
{
	/*
	 *  registers indices
	 */
	Rid=		0x0,		/* identification and revision */
	Ris=		0x1,		/* interface status */
	Rpc=	 	0x2,		/* power control */
	 Foutena=	 (1<<7),	/*  output enable */
	 Fautopower=	 (1<<5),	/*  automatic power switching */
	 Fcardena=	 (1<<4),	/*  PC card enable */
	Rigc= 		0x3,		/* interrupt and general control */
	 Fiocard=	 (1<<5),	/*  I/O card (vs memory) */
	 Fnotreset=	 (1<<6),	/*  reset if not set */	
	 FSMIena=	 (1<<4),	/*  enable change interrupt on SMI */ 
	Rcsc= 		0x4,		/* card status change */
	Rcscic= 	0x5,		/* card status change interrupt config */
	 Fchangeena=	 (1<<3),	/*  card changed */
	 Fbwarnena=	 (1<<1),	/*  card battery warning */
	 Fbdeadena=	 (1<<0),	/*  card battery dead */
	Rwe= 		0x6,		/* address window enable */
	 Fmem16=	 (1<<5),	/*  use A23-A12 to decode address */
	Rio= 		0x7,		/* I/O control */
	Riobtm0lo=	0x8,		/* I/O address 0 start low byte */
	Riobtm0hi=	0x9,		/* I/O address 0 start high byte */
	Riotop0lo=	0xa,		/* I/O address 0 stop low byte */
	Riotop0hi=	0xb,		/* I/O address 0 stop high byte */
	Riobtm1lo=	0xc,		/* I/O address 1 start low byte */
	Riobtm1hi=	0xd,		/* I/O address 1 start high byte */
	Riotop1lo=	0xe,		/* I/O address 1 stop low byte */
	Riotop1hi=	0xf,		/* I/O address 1 stop high byte */
	Rmap=		0x10,		/* map 0 */

	/*
	 *  CL-PD67xx extension registers
	 */
	Rmisc1=		0x16,		/* misc control 1 */
	 F5Vdetect=	 (1<<0),
	 Fvcc3V=	 (1<<1),
	 Fpmint=	 (1<<2),
	 Fpsirq=	 (1<<3),
	 Fspeaker=	 (1<<4),
	 Finpack=	 (1<<7),
	Rfifo=		0x17,		/* fifo control */
	 Fflush=	 (1<<7),	/*  flush fifo */
	Rmisc2=		0x1E,		/* misc control 2 */
	Rchipinfo=	0x1F,		/* chip information */
	Ratactl=	0x26,		/* ATA control */

	/*
	 *  offsets into the system memory address maps
	 */
	Mbtmlo=		0x0,		/* System mem addr mapping start low byte */
	Mbtmhi=		0x1,		/* System mem addr mapping start high byte */
	Mtoplo=		0x2,		/* System mem addr mapping stop low byte */
	Mtophi=		0x3,		/* System mem addr mapping stop high byte */
	 F16bit=	 (1<<7),	/*  16-bit wide data path */
	Mofflo=		0x4,		/* Card memory offset address low byte */
	Moffhi=		0x5,		/* Card memory offset address high byte */
	 Fregactive=	 (1<<6),	/*  attribute meory */

	Mbits=		13,		/* msb of Mchunk */
	Mchunk=		1<<Mbits,	/* logical mapping granularity */
	Nmap=		4,		/* max number of maps to use */

	/*
	 *  configuration registers - they start at an offset in attribute
	 *  memory found in the CIS.
	 */
	Rconfig=	0,
	 Creset=	 (1<<7),	/*  reset device */
	 Clevel=	 (1<<6),	/*  level sensitive interrupt line */
};

#define MAP(x,o)	(Rmap + (x)*0x8 + o)

typedef struct I82365	I82365;
typedef struct Slot	Slot;
typedef struct PCMmap	PCMmap;

/* maps between ISA memory space and the card memory space */
struct PCMmap
{
	ulong	ca;		/* card address */
	ulong	cea;		/* card end address */
	ulong	isa;		/* ISA address */
	int	attr;		/* attribute memory */
	int	time;
};

/* a controller */
enum
{
	Ti82365,
	Tpd6710,
	Tpd6720,
};
struct I82365
{
	QLock;
	int	type;
	int	dev;
	int	nslot;
	int	xreg;		/* index register address */
	int	dreg;		/* data register address */
};
static I82365 *controller[4];
static int ncontroller;

/* a Slot slot */
struct Slot
{
	int	ref;

	I82365	*cp;		/* controller for this slot */
	long	memlen;		/* memory length */
	uchar	base;		/* index register base */
	uchar	dev;		/* slot number */

	/* status */
	uchar	special;	/* in use for a special device */
	uchar	already;	/* already inited */
	uchar	gotmem;		/* already got memmap space */
	uchar	occupied;
	uchar	battery;
	uchar	wrprot;
	uchar	powered;
	uchar	configed;
	uchar	enabled;
	uchar	iocard;
	uchar	busy;

	/* cis info */
	uchar	vpp1;
	uchar	vpp2;
	uchar	bit16;
	uchar	nioregs;
	uchar	memwait;
	uchar	cpresent;	/* config registers present */
	uchar	def;		/* default configuration */
	ushort	irqs;		/* valid interrupt levels */
	ulong	caddr;		/* relative address of config registers */
	uchar	*cisbase;	/* base of mapped in attribute space */
	uchar	*cispos;	/* current position scanning cis */

	/* memory maps */
	int	time;
	PCMmap	mmap[Nmap];
};
static Slot	*slot;
static Slot	*lastslot;
static nslot;

static void cisread(Slot*);

/*
 *  reading and writing card registers
 */
static uchar
rdreg(Slot *pp, int index)
{
	outb(pp->cp->xreg, pp->base + index);
	return inb(pp->cp->dreg);
}
static void
wrreg(Slot *pp, int index, uchar val)
{
	outb(pp->cp->xreg, pp->base + index);
	outb(pp->cp->dreg, val);
}

/*
 *  get info about card
 */
static void
slotinfo(Slot *pp)
{
	uchar isr;

	isr = rdreg(pp, Ris);
	pp->occupied = (isr & (3<<2)) == (3<<2);
	pp->powered = isr & (1<<6);
	pp->battery = (isr & 3) == 3;
	pp->wrprot = isr & (1<<4);
	pp->busy = isr & (1<<5);

	isr = rdreg(pp, Rigc);
	pp->iocard = isr & Fiocard;
}

static int
vcode(int volt)
{
	switch(volt){
	case 5:
		return 1;
	case 12:
		return 2;
	default:
		return 0;
	}
}

/*
 *  enable the slot card
 */
static void
slotena(Slot *pp)
{
	int x;

	if(pp->enabled)
		return;

	if(pp->already == 0){
		pp->already = 1;

		/* interrupt on card status change */
		wrreg(pp, Rigc, Fnotreset);
		wrreg(pp, Rcscic, ((PCMCIAvec-Int0vec)<<4) | Fchangeena
			| Fbwarnena | Fbdeadena);
	}

	/* display status */
	slotinfo(pp);
	if(pp->occupied){
		/* enable the card */
		wrreg(pp, Rpc, vcode(5)|Fautopower|Foutena|Fcardena);
		pp->enabled = 1;
		cisread(pp);

		/* set real power values if we configured successfully */
		if(pp->configed){
			x = vcode(pp->vpp1) | (vcode(pp->vpp2)<<2);
			wrreg(pp, Rpc, x|Fautopower|Foutena|Fcardena);
		}

	}
}

/*
 *  disable the slot card
 */
static void
slotdis(Slot *pp)
{
	/* disable the windows into the card */
	wrreg(pp, Rwe, 0);

	/* disable the card */
	wrreg(pp, Rpc, 5|Fautopower);
	pp->enabled = 0;
}

/*
 *  status change interrupt
 */
static void
i82365intr(Ureg *ur)
{
	uchar csc;
	Slot *pp;

	USED(ur);
	for(pp = slot; pp < lastslot; pp++){
		csc = rdreg(pp, Rcsc);
		slotinfo(pp);
		if(csc & 1)
			print("slot card %d battery dead\n", pp->dev);
		if(csc & (1<<1))
			print("slot card %d battery warning\n", pp->dev);
		if(csc & (1<<3)){
			if(pp->occupied && pp->ref){
				print("slot card %d inserted\n", pp->dev);
				slotena(pp);
			} else {
				print("slot card %d removed\n", pp->dev);
				slotdis(pp);
			}
		}
	}
}

/*
 *  get a map for pc card region, return corrected len
 */
static PCMmap*
getmap(Slot *pp, ulong offset, int attr)
{
	uchar we, bit;
	PCMmap *m, *lru;
	int i;

	if(pp->gotmem == 0){
		pp->gotmem = 1;

		/* grab ISA address space for memory maps */
		for(i = 0; i < Nmap; i++)
			pp->mmap[i].isa = getisa(0, Mchunk, BY2PG);
		if(pp->mmap[i].isa == 0)
			panic("getmap");
	}

	/* look for a map that starts in the right place */
	we = rdreg(pp, Rwe);
	bit = 1;
	lru = pp->mmap;
	for(m = pp->mmap; m < &pp->mmap[Nmap]; m++){
		if((we & bit) && m->attr == attr && offset >= m->ca && offset < m->cea){
			m->time = pp->time++;
			return m;
		}
		bit <<= 1;
		if(lru->time > m->time)
			lru = m;
	}

	/* use the least recently used */
	m = lru;
	offset &= ~(Mchunk - 1);
	m->ca = offset;
	m->cea = m->ca + Mchunk;
	m->attr = attr;
	m->time = pp->time++;
	i = m - pp->mmap;
	bit = 1<<i;
	wrreg(pp, Rwe, we & ~bit);		/* disable map before changing it */
	wrreg(pp, MAP(i, Mbtmlo), m->isa>>12);
	wrreg(pp, MAP(i, Mbtmhi), (m->isa>>(12+8)) | F16bit);
	wrreg(pp, MAP(i, Mtoplo), (m->isa+Mchunk-1)>>12);
	wrreg(pp, MAP(i, Mtophi), (m->isa+Mchunk-1)>>(12+8));
	offset -= m->isa;
	offset &= (1<<25)-1;
	offset >>= 12;
	wrreg(pp, MAP(i, Mofflo), offset);
	wrreg(pp, MAP(i, Moffhi), (offset>>8) | (attr ? Fregactive : 0));
	wrreg(pp, Rwe, we | bit);		/* enable map */
	return m;
}

static void
increfp(Slot *pp)
{
	qlock(pp->cp);
	if(pp->ref++ == 0)
		slotena(pp);
	qunlock(pp->cp);
}

static void
decrefp(Slot *pp)
{
	qlock(pp->cp);
	if(pp->ref-- == 1)
		slotdis(pp);
	qunlock(pp->cp);
}

int
pcmspecial(int dev)
{
	Slot *pp;

	i82365reset();
	if(dev >= nslot)
		return -1;
	pp = slot + dev;
	if(pp->special)
		return -1;
	increfp(pp);
	if(!pp->occupied){
		decrefp(pp);
		return -1;
	}
	pp->special = 1;
	return 0;
}

void
pcmspecialclose(int dev)
{
	Slot *pp;

	if(dev >= nslot)
		panic("pcmspecialclose");
	pp = slot + dev;
	pp->special = 0;
	decrefp(pp);
}

enum
{
	Qdir,
	Qmem,
	Qattr,
	Qctl,
};

#define DEV(c)	(c->qid.path>>8)
#define TYPE(c)	(c->qid.path&0xff)

static int
pcmgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	int dev;
	Qid qid;
	long len;
	Slot *pp;
	char name[NAMELEN];

	USED(tab, ntab);
	if(i>=3*nslot)
		return -1;
	dev = i/3;
	pp = slot + dev;
	len = 0;
	switch(i%3){
	case 0:
		qid.path = (dev<<8) | Qmem;
		sprint(name, "pcm%dmem", dev);
		len = pp->memlen;
		break;
	case 1:
		qid.path = (dev<<8) | Qattr;
		sprint(name, "pcm%dattr", dev);
		len = pp->memlen;
		break;
	case 2:
		qid.path = (dev<<8) | Qctl;
		sprint(name, "pcm%dctl", dev);
		break;
	}
	qid.vers = 0;
	devdir(c, qid, name, len, eve, 0660, dp);
	return 1;
}

static char *chipname[] =
{
[Ti82365]	"Intel 82365SL",
[Tpd6710]	"Cirrus Logic PD6710",
[Tpd6720]	"Cirrus Logic PD6720",
};

static I82365*
i82386probe(int x, int d, int dev)
{
	uchar c;
	I82365 *cp;

	outb(x, Rid + (dev<<7));
	c = inb(d);
	if((c & 0xf0) != 0x80)
		return 0;		/* not this family */

	cp = xalloc(sizeof(I82365));
	cp->xreg = x;
	cp->dreg = d;
	cp->dev = dev;
	cp->type = Ti82365;
	cp->nslot = 2;

	switch(c){
	case 0x82:
	case 0x83:
		/* could be a cirrus */
		outb(x, Rchipinfo + (dev<<7));
		outb(d, 0);
		c = inb(d);
		if((c & 0xdf) == 0xdc){
			c = inb(d);
			if((c & 0xdf) != 0x0c)
				break;
		}
		if(c & 0x40){
			cp->type = Tpd6720;
		} else {
			cp->type = Tpd6710;
			cp->nslot = 1;
		}
		break;
	}

	print("pcmcia controller%d is a %d slot %s\n", ncontroller, cp->nslot,
		chipname[cp->type]);

	controller[ncontroller++] = cp;
	return cp;
}

/*
 *  set up for slot cards
 */
void
i82365reset(void)
{
	static int already;
	int i, j;
	I82365 *cp;

	if(already)
		return;
	already = 1;

	/* look for controllers */
	i82386probe(0x3E0, 0x3E1, 0);
	i82386probe(0x3E0, 0x3E1, 1);
	for(i = 0; i < ncontroller; i++)
		nslot += controller[i]->nslot;
	slot = xalloc(nslot * sizeof(Slot));

	/* if the card is there turn on 5V power to keep its battery alive */
	lastslot = slot;
	for(i = 0; i < ncontroller; i++){
		cp = controller[i];
		for(j = 0; j < cp->nslot; j++){
			lastslot->dev = lastslot - slot;
			lastslot->memlen = 64*MB;
			lastslot->base = (cp->dev<<7) | (j<<6);
			lastslot->cp = cp;
			wrreg(lastslot, Rpc, 5|Fautopower);
			lastslot++;
		}
	}
}

void
i82365init(void)
{
}

Chan *
i82365attach(char *spec)
{
	return devattach('y', spec);
}

Chan *
i82365clone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
i82365walk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, pcmgen);
}

void
i82365stat(Chan *c, char *db)
{
	devstat(c, db, 0, 0, pcmgen);
}

Chan *
i82365open(Chan *c, int omode)
{
	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	} else
		increfp(slot + DEV(c));
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
i82365create(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
i82365remove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
i82365wstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
i82365close(Chan *c)
{
	USED(c);
	if(c->qid.path != CHDIR)
		decrefp(slot+DEV(c));
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

long
pcmread(int dev, int attr, void *a, long n, ulong offset)
{
	int i, len;
	PCMmap *m;
	ulong ka;
	uchar *ac;
	Slot *pp;

	pp = slot + dev;
	if(pp->memlen < offset)
		return 0;
	qlock(pp->cp);
	ac = a;
	if(pp->memlen < offset + n)
		n = pp->memlen - offset;
	for(len = n; len > 0; len -= i){
		if(pp->occupied == 0 || pp->enabled == 0)
			error(Eio);
		m = getmap(pp, offset, attr);
		if(offset + len > m->cea)
			i = m->cea - offset;
		else
			i = len;
		ka = KZERO|(m->isa + (offset&(Mchunk-1)));
		memmoves(ac, (void*)ka, i);
		offset += i;
		ac += i;
	}
	qunlock(pp->cp);
	return n;
}

long
i82365read(Chan *c, void *a, long n, ulong offset)
{
	char *cp, buf[128];
	ulong p;
	Slot *pp;

	p = TYPE(c);
	switch(p){
	case Qdir:
		return devdirread(c, a, n, 0, 0, pcmgen);
	case Qmem:
	case Qattr:
		n = pcmread(DEV(c), p==Qattr, a, n, offset);
		break;
	case Qctl:
		cp = buf;
		pp = slot + DEV(c);
		if(pp->occupied)
			cp += sprint(cp, "occupied\n");
		if(pp->enabled)
			cp += sprint(cp, "enabled\n");
		if(pp->powered)
			cp += sprint(cp, "powered\n");
		if(pp->iocard)
			cp += sprint(cp, "iocard\n");
		if(pp->configed)
			cp += sprint(cp, "configed\n");
		if(pp->wrprot)
			cp += sprint(cp, "write protected\n");
		if(pp->busy)
			cp += sprint(cp, "busy\n");
		cp += sprint(cp, "battery lvl %d\n", pp->battery);
		*cp = 0;
		return readstr(offset, a, n, buf);
	default:
		n=0;
		break;
	}
	return n;
}

long
pcmwrite(int dev, int attr, void *a, long n, ulong offset)
{
	int i, len;
	PCMmap *m;
	ulong ka;
	uchar *ac;
	Slot *pp;

	pp = slot + dev;
	if(pp->memlen < offset)
		return 0;
	qlock(pp->cp);
	ac = a;
	if(pp->memlen < offset + n)
		n = pp->memlen - offset;
	for(len = n; len > 0; len -= i){
		m = getmap(pp, offset, attr);
		if(offset + len > m->cea)
			i = m->cea - offset;
		else
			i = len;
		ka = KZERO|(m->isa + (offset&(Mchunk-1)));
		memmoves((void*)ka, ac, i);
		offset += i;
		ac += i;
	}
	qunlock(pp->cp);
	return n;
}

long
i82365write(Chan *c, void *a, long n, ulong offset)
{
	ulong p;
	Slot *pp;

	p = TYPE(c);
	switch(p){
	case Qmem:
	case Qattr:
		pp = slot + DEV(c);
		if(pp->occupied == 0 || pp->enabled == 0)
			error(Eio);
		n = pcmwrite(pp->dev, p == Qattr, a, n, offset);
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

/*
 *  configure the Slot for IO.  We assume very heavily that we can read
 *  cofiguration info from the CIS.  If not, we won't set up correctly.
 */
int
pcmio(int dev, ISAConf *isa)
{
	uchar we, x;
	Slot *pp;

	if(dev > nslot)
		return -1;
	pp = slot + dev;

	if(!pp->occupied)
		return -1;

	/* if no io registers, assume not an io card (iffy!) */
	if(pp->nioregs == 0)
		return -1;

	/* route interrupts, make sure card can use specified interrupt */
	if(isa->irq == 2)
		isa->irq = 9;
	if(((1<<isa->irq) & pp->irqs) == 0)
		return -1;
	wrreg(pp, Rigc, isa->irq | Fnotreset | Fiocard);
	
	/* set power and enable device */
	x = vcode(pp->vpp1) | (vcode(pp->vpp2)<<2);
	wrreg(pp, Rpc, x|Fautopower|Foutena|Fcardena);

	/* 16-bit data path */
	if(pp->bit16)
		wrreg(pp, Rio, (1<<0)|(1<<1));

	/* enable io port map 0 */
	if(isa->port == 0)
		isa->port = 300;
	we = rdreg(pp, Rwe);
	wrreg(pp, Riobtm0lo, isa->port);
	wrreg(pp, Riobtm0hi, isa->port>>8);
	wrreg(pp, Riotop0lo, (isa->port+pp->nioregs));
	wrreg(pp, Riotop0hi, (isa->port+pp->nioregs)>>8);
	wrreg(pp, Rwe, we | (1<<6));

	/* only touch Rconfig if it is present */
	if(pp->cpresent & (1<<Rconfig)){
		/*  Reset adapter */
		x = Creset;
		pcmwrite(dev, 1, &x, 1, pp->caddr + Rconfig);
		delay(2);
		x = 0;
		pcmwrite(dev, 1, &x, 1, pp->caddr + Rconfig);
		delay(2);
	
		/*
		 *  Set level sensitive (not pulsed) interrupts and
		 *  configuration number 1.
		 *  Setting the configuration number enables IO port access.
		 */
		x = Clevel | 1;
		pcmwrite(dev, 1, &x, 1, pp->caddr + Rconfig);
		delay(2);
	}
	return 0;
}

/*
 *  read and crack the card information structure enough to set
 *  important parameters like power
 */
static void	tcfig(Slot*, int);
static void	tentry(Slot*, int);

static void (*parse[256])(Slot*, int) =
{
[0x1A]	tcfig,
[0x1B]	tentry,
};

static int
readc(Slot *pp, uchar *x)
{
	uchar l, r;
	ushort s;

	if(pp->cispos > pp->cisbase + Mchunk)
		return 0;

	*x = *(pp->cispos);
	pp->cispos += 2;
	return 1;
}

static void
cisread(Slot *pp)
{
	PCMmap *m;
	uchar link;
	uchar type;
	uchar *this;
	int i;

	pp->vpp1 = pp->vpp2 = 5;
	pp->bit16 = 0;
	pp->caddr = 0;
	pp->cpresent = 0;
	pp->def = 0;
	pp->irqs = 0xffff;

	/* map in the attribute memory cis should be in */
	m = getmap(pp, 0, 1);
	pp->cispos = pp->cisbase = (uchar*)(KZERO|m->isa);

	/* loop through all the tuples */
	for(i = 0; i < 1000; i++){
		this = pp->cispos;
		if(readc(pp, &type) != 1)
			break;
		if(readc(pp, &link) != 1)
			break;
		if(parse[type])
			(*parse[type])(pp, type);
		if(link == 0xff)
			break;
		pp->cispos = this + 2*(2+link);
		if(this > pp->cisbase + Mchunk)
			break;
	}
}

static ulong
getlong(Slot *pp, int size)
{
	uchar c;
	int i;
	ulong x;

	x = 0;
	for(i = 0; i < size; i++){
		if(readc(pp, &c) != 1)
			break;
		x |= c<<(i*8);
	}
	return x;
}

static void
tcfig(Slot *pp, int ttype)
{
	uchar size, rasize, rmsize;
	uchar last;

	USED(ttype);
	if(readc(pp, &size) != 1)
		return;
	rasize = (size&0x3) + 1;
	rmsize = ((size>>2)&0xf) + 1;
	if(readc(pp, &last) != 1)
		return;
	pp->caddr = getlong(pp, rasize);
	pp->cpresent = getlong(pp, rmsize);
}

static ulong vexp[8] =
{
	1, 10, 100, 1000, 10000, 100000, 1000000, 10000000
};
static ulong vmant[16] =
{
	10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80, 90,
};

static ulong
microvolt(Slot *pp)
{
	uchar c;
	ulong microvolts;

	if(readc(pp, &c) != 1)
		return 0;
	microvolts = vexp[c&0x7]*vmant[(c>>3)&0xf];
	while(c & 0x80){
		if(readc(pp, &c) != 1)
			return 0;
		if(c == 0x7d || c == 0x7e || c == 0x7f)
			microvolts = 0;
	}
	return microvolts;
}

static ulong
nanoamps(Slot *pp)
{
	uchar c;
	ulong nanoamps;

	if(readc(pp, &c) != 1)
		return 0;
	nanoamps = vexp[c&0x7]*vmant[(c>>3)&0xf];
	while(c & 0x80){
		if(readc(pp, &c) != 1)
			return 0;
		if(c == 0x7d || c == 0x7e || c == 0x7f)
			nanoamps = 0;
	}
	return nanoamps;
}

/*
 *  only nominal voltage is important for config
 */
static ulong
power(Slot *pp)
{
	uchar feature;
	ulong mv;

	mv = 0;
	if(readc(pp, &feature) != 1)
		return 0;
	if(feature & 1)
		mv = microvolt(pp);
	if(feature & 2)
		microvolt(pp);
	if(feature & 4)
		microvolt(pp);
	if(feature & 8)
		nanoamps(pp);
	if(feature & 0x10)
		nanoamps(pp);
	if(feature & 0x20)
		nanoamps(pp);
	if(feature & 0x20)
		nanoamps(pp);
	return mv/1000000;
}

static ulong mantissa[16] =
{ 0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80, };

static ulong exponent[8] =
{ 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, };

static ulong
ttiming(Slot *pp, int scale)
{
	uchar unscaled;
	ulong nanosecs;

	if(readc(pp, &unscaled) != 1)
		return 0;
	nanosecs = (mantissa[(unscaled>>3)&0xf]*exponent[unscaled&7])/10;
	nanosecs = nanosecs * vexp[scale];
	return nanosecs;
}

static void
timing(Slot *pp)
{
	uchar c, i;

	if(readc(pp, &c) != 1)
		return;
	i = c&0x3;
	if(i != 3)
		ttiming(pp, i);		/* max wait */
	i = (c>>2)&0x7;
	if(i != 7)
		ttiming(pp, i);		/* max ready/busy wait */
	i = (c>>5)&0x7;
	if(i != 7)
		ttiming(pp, i);		/* reserved wait */
}

void
iospaces(Slot *pp)
{
	uchar c;
	int i;
	ulong address, len;

	for(;;){
		if(readc(pp, &c) != 1)
			break;

		pp->nioregs = 1<<(c&0x1f);
		pp->bit16 = ((c>>5)&3) >= 2;
		if((c & 0x80) == 0)
			break;

		if(readc(pp, &c) != 1)
			break;

		for(i = (c&0xf)+1; i; i--){
			address = getlong(pp, (c>>4)&0x3);
			len = getlong(pp, (c>>6)&0x3);
			USED(address, len);
		}
	}
}

static void
irq(Slot *pp)
{
	uchar c;

	if(readc(pp, &c) != 1)
		return;
	if(c & 0x10)
		pp->irqs = getlong(pp, 2);
	else
		pp->irqs = 1<<(c&0xf);
}

static void
memspace(Slot *pp, int asize, int lsize, int host)
{
	ulong haddress, address, len;

	len = getlong(pp, lsize)*256;
	address = getlong(pp, asize)*256;
	USED(len, address);
	if(host){
		haddress = getlong(pp, asize)*256;
		USED(haddress);
	}
}

void
tentry(Slot *pp, int ttype)
{
	uchar c, i, feature;

	USED(ttype);
	if(readc(pp, &c) != 1)
		return;
	if(c&0x40)
		pp->def = 1;
	if(c & 0x80){
		if(readc(pp, &i) != 1)
			return;
		if(i&0x80)
			pp->memwait = 1;
	}
	if(readc(pp, &feature) != 1)
		return;
	switch(feature&0x3){
	case 1:
		pp->vpp1 = pp->vpp2 = power(pp);
		break;
	case 2:
		power(pp);
		pp->vpp1 = pp->vpp2 = power(pp);
		break;
	case 3:
		power(pp);
		pp->vpp1 = power(pp);
		pp->vpp2 = power(pp);
		break;
	default:
		break;
	}
	if(feature&0x4)
		timing(pp);
	if(feature&0x8)
		iospaces(pp);
	if(feature&0x10)
		irq(pp);
	switch(feature&0x3){
	case 1:
		memspace(pp, 0, 2, 0);
		break;
	case 2:
		memspace(pp, 2, 2, 0);
		break;
	case 3:
		if(readc(pp, &c) != 1)
			return;
		for(i = 0; i <= c&0x7; i++)
			memspace(pp, (c>>5)&0x3, (c>>3)&0x3, c&0x80);
		break;
	}
	pp->configed++;
}
