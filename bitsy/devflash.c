#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

/*
 *  on the bitsy, all 32 bit accesses to flash are mapped to two 16 bit
 *  accesses, one to the low half of the chip and the other to the high
 *  half.  Therefore for all command accesses, ushort indices in the
 *  manuals turn into ulong indices in our code.  Also, by copying all
 *  16 bit commands to both halves of a 32 bit command, we erase 2
 *  sectors for each request erase request.
 */

#define mirror(x) (((x)<<16)|(x))

/* this defines a contiguous set of erase blocks of one size */
typedef struct FlashRegion FlashRegion;
struct FlashRegion
{
	ulong	addr;		/* start of region */
	ulong	end;		/* end of region + 1 */
	ulong	n;		/* number of blocks */
	ulong	size;		/* size of each block */
};

/* this defines a particular access algorithm */
typedef struct FlashAlg FlashAlg;
struct FlashAlg
{
	int	id;
	char	*name;
	void	(*identify)(void);	/* identify device */
	void	(*erase)(ulong);	/* erase a region */
	void	(*write)(void*, long, ulong);	/* write a region */
};

static void	ise_id(void);
static void	ise_erase(ulong);
static void	ise_write(void*, long, ulong);

static void	afs_id(void);
static void	afs_erase(ulong);
static void	afs_write(void*, long, ulong);

FlashAlg falg[] =
{
	{ 1,	"Intel/Sharp Extended",	ise_id, ise_erase, ise_write	},
	{ 2,	"AMD/Fujitsu Standard",	afs_id, afs_erase, afs_write	},
};

struct
{
	RWlock;
	ulong		*p;
	ushort		algid;		/* access algorithm */
	FlashAlg	*alg;
	ushort		manid;		/* manufacturer id */
	ushort		devid;		/* device id */
	ulong		size;		/* size in bytes */
	int		wbsize;		/* size of write buffer */ 
	ulong		nr;		/* number of regions */
	uchar		bootprotect;
	FlashRegion	r[32];
	ulong		*wb;		/* staging area for write buffer */
} flash;

/*
 *  common flash interface
 */
static uchar
cfigetc(int off)
{
	uchar rv;

	flash.p[0x55] = mirror(0x98);
	rv = flash.p[off];
	flash.p[0x55] = mirror(0xFF);
	return rv;
}

static ushort
cfigets(int off)
{
	return (cfigetc(off+1)<<8)|cfigetc(off);
}

static ulong
cfigetl(int off)
{
	return (cfigetc(off+3)<<24)|(cfigetc(off+2)<<16)|
		(cfigetc(off+1)<<8)|cfigetc(off);
}

static void
cfiquery(void)
{
	uchar q, r, y;
	ulong x, addr;

	q = cfigetc(0x10);
	r = cfigetc(0x11);
	y = cfigetc(0x12);
	if(q != 'Q' || r != 'R' || y != 'Y'){
		print("cfi query failed: %ux %ux %ux\n", q, r, y);
		return;
	}
	flash.algid = cfigetc(0x13);
	flash.size = 1<<(cfigetc(0x27)+1);
	flash.wbsize = 1<<(cfigetc(0x2a)+1);
	flash.nr = cfigetc(0x2c);
	if(flash.nr > nelem(flash.r)){
		print("cfi reports > %d regions\n", nelem(flash.r));
		flash.nr = nelem(flash.r);
	}
	addr = 0;
	for(q = 0; q < flash.nr; q++){
		x = cfigetl(q+0x2d);
		flash.r[q].size = 2*256*(x>>16);
		flash.r[q].n = (x&0xffff)+1;
		flash.r[q].addr = addr;
		addr += flash.r[q].size*flash.r[q].n;
		flash.r[q].end = addr;
	}
	flash.wb = malloc(flash.wbsize);
}

/*
 *  flash device interface
 */

enum
{
	Qfctl=1,
	Qfdata,
};

Dirtab flashdir[]={
	"flashctl",		{ Qfctl, 0 },	0,	0664,
	"flashdata",		{ Qfdata, 0 },	0,	0660,
};

void
flashinit(void)
{
	int i;

	flash.p = (ulong*)FLASHZERO;
	cfiquery();
	for(i = 0; i < nelem(falg); i++)
		if(flash.algid == falg[i].id){
			flash.alg = &falg[i];
			(*flash.alg->identify)();
			break;
		}
	flash.bootprotect = 1;
}

static Chan*
flashattach(char* spec)
{
	return devattach('F', spec);
}

static int	 
flashwalk(Chan* c, char* name)
{
	return devwalk(c, name, flashdir, nelem(flashdir), devgen);
}

static void	 
flashstat(Chan* c, char* dp)
{
	devstat(c, dp, flashdir, nelem(flashdir), devgen);
}

static Chan*
flashopen(Chan* c, int omode)
{
	omode = openmode(omode);
	if(strcmp(up->user, eve)!=0)
		error(Eperm);
	return devopen(c, omode, flashdir, nelem(flashdir), devgen);
}

static void	 
flashclose(Chan*)
{
}

static long	 
flashread(Chan* c, void* a, long n, vlong off)
{
	char *buf, *p, *e;
	int i;

	if(c->qid.path&CHDIR)
		return devdirread(c, a, n, flashdir, nelem(flashdir), devgen);
	switch(c->qid.path){
	default:
		error(Eperm);
	case Qfctl:
		buf = smalloc(1024);
		e = buf + 1024;
		p = seprint(buf, e, "0x%-9lux 0x%-9lux 0x%-9lux 0x%-9lux\n", flash.size,
			flash.wbsize, flash.manid, flash.devid);
		for(i = 0; i < flash.nr; i++)
			p = seprint(p, e, "0x%-9lux 0x%-9lux 0x%-9lux\n", flash.r[i].addr,
				flash.r[i].n, flash.r[i].size);
		n = readstr(off, a, n, buf);
		free(buf);
		break;
	case Qfdata:
		if(!iseve())
			error(Eperm);
		if(off >= flash.size)
			return 0;
		if(off + n > flash.size)
			n = flash.size - off;
		rlock(&flash);
		if(waserror()){
			runlock(&flash);
			nexterror();
		}
		memmove(a, ((uchar*)FLASHZERO)+off, n);
		runlock(&flash);
		poperror();
		break;
	}
	return n;
}

static void
bootprotect(ulong addr)
{
	FlashRegion *r;

	if(flash.bootprotect == 0)
		return;
	if(flash.nr == 0)
		error("writing over boot loader disallowed");
	r = flash.r;
	if(addr >= r->addr && addr < r->addr + r->size)
		error("writing over boot loader disallowed");
}

ulong
blockstart(ulong addr)
{
	FlashRegion *r, *e;
	ulong x;

	r = flash.r;
	for(e = &flash.r[flash.nr]; r < e; r++)
		if(addr >= r->addr && addr < r->end){
			x = addr - r->addr;
			x /= r->size;
			return r->addr + x*r->size;
		}
			
	return (ulong)-1;
}

ulong
blockend(ulong addr)
{
	FlashRegion *r, *e;
	ulong x;

	r = flash.r;
	for(e = &flash.r[flash.nr]; r < e; r++)
		if(addr >= r->addr && addr < r->end){
			x = addr - r->addr;
			x /= r->size;
			return r->addr + (x+1)*r->size;
		}
			
	return (ulong)-1;
}

static long
flashctlwrite(char *p, long n)
{
	Cmdbuf *cmd;
	ulong addr;

	cmd = parsecmd(p, n);
	wlock(&flash);
	if(waserror()){
		wunlock(&flash);
		nexterror();
	}
	if(strcmp(cmd->f[0], "erase") == 0){
		if(cmd->nf != 2)
			error(Ebadarg);
		addr = atoi(cmd->f[1]);
		if(addr != blockstart(addr))
			error("erase must be a block boundary");
		bootprotect(addr);
		(*flash.alg->erase)(addr);
	} else if(strcmp(cmd->f[0], "protectboot") == 0){
		if(cmd->nf == 0 || strcmp(cmd->f[1], "off") != 0)
			flash.bootprotect = 1;
		else
			flash.bootprotect = 0;
	} else
		error(Ebadarg);
	poperror();
	wunlock(&flash);
	free(cmd);

	return n;
}

static long
flashdatawrite(uchar *p, long n, long off)
{
	uchar *end;
	int m;
	long ooff = off;
	uchar *op = p;

	if((off & 0x3) || (n & 0x3))
		error("only quad writes");
	if(off >= flash.size || off+n > flash.size || n <= 0)
		error(Ebadarg);

	wlock(&flash);
	if(waserror()){
		wunlock(&flash);
		nexterror();
	}

	/* make sure we're not writing the boot sector */
	bootprotect(off);

	/* (*flash.alg->write) can't cross blocks */
	for(end = p + n; p < end; p += m){
		m = blockend(off) - off;
		if(m > end - p)
			m = end - p;
		(*flash.alg->write)(p, m, off);
		off += m;
	}

	/* make sure write succeeded */
	if(memcmp(op, &flash.p[ooff>>2], n) != 0)
		error("written bytes don't match");

	wunlock(&flash);
	poperror();

	return n;
}

static long	 
flashwrite(Chan* c, void* a, long n, vlong off)
{
	if(c->qid.path & CHDIR)
		error(Eperm);

	if(!iseve())
		error(Eperm);

	switch(c->qid.path){
	default:
		panic("flashwrite");
	case Qfctl:
		return flashctlwrite(a, n);
	case Qfdata:
		return flashdatawrite(a, n, off);
	}
	return n;
}

Dev flashdevtab = {
	'F',
	"flash",

	devreset,
	flashinit,
	flashattach,
	devclone,
	flashwalk,
	flashstat,
	flashopen,
	devcreate,
	flashclose,
	flashread,
	devbread,
	flashwrite,
	devbwrite,
	devremove,
	devwstat,
};


/* intel/sharp extended command set */
static void
ise_reset(void)
{
	flash.p[0x55] = mirror(0xff);	/* reset */
}
static void
ise_id(void)
{
	ise_reset();
	flash.p[0x555] = mirror(0x90);	/* uncover vendor info */
	flash.manid = flash.p[00];
	flash.devid = flash.p[01];
	ise_reset();
}
static void
ise_clearerror(void)
{
	flash.p[0x100] = mirror(0x50);

}
static void
ise_error(int bank, ulong status)
{
	char err[ERRLEN];

	if(status & (1<<3)){
		sprint(err, "flash%d: low prog voltage", bank);
		error(err);
	}
	if(status & (1<<1)){
		sprint(err, "flash%d: block locked", bank);
		error(err);
	}
	if(status & (1<<5)){
		sprint(err, "flash%d: i/o error", bank);
		error(err);
	}
}
static void
ise_erase(ulong addr)
{
	ulong start;
	ulong x;

	addr >>= 2;	/* convert to ulong offset */

	flashprogpower(1);
	flash.p[addr] = mirror(0x20);
	flash.p[addr] = mirror(0xd0);
	start = m->ticks;
	do {
		x = flash.p[addr];
		if((x & mirror(1<<7)) == mirror(1<<7))
			break;
	} while(TK2MS(m->ticks-start) < 1500);
	flashprogpower(0);

	ise_clearerror();
	ise_error(0, x);
	ise_error(1, x>>16);

	ise_reset();
}
/*
 *  flash writing goes about 16 times faster if we use
 *  the write buffer.  We fill the write buffer and then
 *  issue the write request.  After the write request,
 *  subsequent reads will yield the status register or,
 *  since error bits are sticky, another write buffer can
 *  be filled and written.
 *
 *  On timeout, we issue a read status register request so
 *  that the status register can be read no matter how we
 *  exit.
 */
static int
ise_wbwrite(ulong *p, int n, ulong off)
{
	ulong start;
	int i;

	/* copy out of user space to avoid faults later */
	memmove(flash.wb, p, n*4);
	p = flash.wb;

	/* put flash into write buffer mode */
	start = m->ticks;
	for(;;) {
		/* request write buffer mode */
		flash.p[off] = mirror(0xe8);

		/* look at extended status reg for status */
		if((flash.p[off] & mirror(1<<7)) == mirror(1<<7))
			break;

		/* didn't work, keep trying for 2 secs */
		if(TK2MS(m->ticks-start) > 2000){
			/* set up to read status */
			flash.p[off] = mirror(0x70);
			return -1;
		}
	}

	/* fill write buffer */
	flash.p[off] = mirror(n-1);
	for(i = 0; i < n; i++)
		flash.p[off+i] = *p++;

	/* program from buffer */
	flash.p[off] = mirror(0xd0);

	/* subsequent reads will return status about the write */

	return n;
}
static void
ise_write(void *a, long n, ulong off)
{
	ulong *p, *end;
	int i, wbsize;
	ulong x, start, ooff;

	/* everything in terms of ulongs */
	wbsize = flash.wbsize>>2;
	off >>= 2;
	n >>= 2;
	p = a;
	ooff = off;

	/* first see if write will succeed */
	for(i = 0; i < n; i++)
		if((p[i] & flash.p[off+i]) != p[i])
			error("flash needs erase");

	if(waserror()){
		ise_reset();
		flashprogpower(0);
		nexterror();
	}
	flashprogpower(1);

	/*
	 *  use the first write to reach
 	 *  a write buffer boundary.  the intel maunal
	 *  says writes startng at wb boundaries
	 *  maximize speed.
	 */
	i = wbsize - (off & (wbsize-1));
	for(end = p + n; p < end;){
		if(i > end - p)
			i = end - p;

		if(ise_wbwrite(p, i, off) != i)
			break;

		off += i;
		p += i;
		i = wbsize;
	}

	/* wait till the programming is done */
	start = m->ticks;
	do {
		x = flash.p[ooff];
		if((x & mirror(1<<7)) == mirror(1<<7))
			break;
	} while(TK2MS(m->ticks-start) < 1000);

	ise_clearerror();
	ise_error(0, x);
	ise_error(1, x>>16);

	ise_reset();
	flashprogpower(0);
	poperror();
}

/* amd/fujitsu standard command set
 *	I don't have an amd chipset to work with
 *	so I'm loathe to write this yet.  If someone
 *	else does, please send it to me and I'll
 *	incorporate it -- presotto@bell-labs.com
 */
static void
afs_reset(void)
{
	flash.p[0x55] = mirror(0xf0);	/* reset */
}
static void
afs_id(void)
{
	afs_reset();
	flash.p[0x55] = mirror(0xf0);	/* reset */
	flash.p[0x555] = mirror(0xaa);	/* query vendor block */
	flash.p[0x2aa] = mirror(0x55);
	flash.p[0x555] = mirror(0x90);
	flash.manid = flash.p[00];
	afs_reset();
	flash.p[0x555] = mirror(0xaa);	/* query vendor block */
	flash.p[0x2aa] = mirror(0x55);
	flash.p[0x555] = mirror(0x90);
	flash.devid = flash.p[01];
	afs_reset();
}
static void
afs_erase(ulong)
{
	error("amd/fujistsu erase not implemented");
}
static void
afs_write(void*, long, ulong)
{
	error("amd/fujistsu write not implemented");
}
