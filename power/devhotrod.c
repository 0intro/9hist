#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

typedef struct Hotrod	Hotrod;
typedef struct Device	Device;
typedef struct Printbuf	Printbuf;

enum {
	Vmevec=		0xd2,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Nhotrod=	2,
};

/*
 *  circular 2 pointer queue for hotrod prnt's
 */
struct Printbuf {
	char	*rptr;
	char	*wptr;
	char	*lim;
	char	buf[4*1024];
};

/*
 *  the hotrod fiber interface responds to 1MB
 *  of either user or supervisor accesses at:
 *  	0x30000000 to 0x300FFFFF  in	A32 space
 *  and	  0xB00000 to   0xBFFFFF  in	A24 space
 */
struct Device {
	ulong	mem[1024*1024/sizeof(ulong)];
};
#define HOTROD		VMEA24SUP(Device, 0xB00000)

struct Hotrod {
	QLock;

	Device		*addr;		/* address of the device */
	int		vec;		/* vme interrupt vector */
	char		name[NAMELEN];	/* hot rod name */
	Printbuf	pbuf;		/* circular queue for hotrod print's */
	int		kprocstarted;
	Rendez		r;
};

Hotrod hotrod[Nhotrod];

void	hotrodintr(int);
void	hotrodkproc(void *a);

int
hotrodgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	if(i || c->dev>=Nhotrod)
		return -1;
	devdir(c, (Qid){0,0}, hotrod[c->dev].name, sizeof(Device), 0666, dp);
	return 1;
}

/*
 *  reset all hotrod boards
 */
void
hotrodreset(void)
{
	int i;
	Hotrod *hp;

	for(i=0; i<Nhotrod; i++){
		hotrod[i].addr = HOTROD+i;
		hotrod[i].vec = Vmevec+i;
		sprint(hotrod[i].name, "hotrod%d", i);
		setvmevec(hotrod[i].vec, hotrodintr);
	}	
	wbflush();
	delay(20);
}

void
hotrodinit(void)
{
}

/*
 *  enable the device for interrupts, spec is the device number
 */
Chan*
hotrodattach(char *spec)
{
	Hotrod *hp;
	int i;
	Chan *c;

	i = strtoul(spec, 0, 0);
	if(i >= Nhotrod)
		error(Ebadarg);

	hp = &hotrod[i];
	if(hp->kprocstarted == 0)
		kproc(hp->name, hotrodkproc, hp);

	c = devattach('H', spec);
	c->dev = i;
	c->qid.path = CHDIR;
	c->qid.vers = 0;
	return c;
}

Chan*
hotrodclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
hotrodwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, hotrodgen);
}

void	 
hotrodstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, hotrodgen);
}

Chan*
hotrodopen(Chan *c, int omode)
{
	Device *dp;
	Hotrod *hp;

#ifdef asdf
	/*
	 *  Remind hotrod where the print buffer is.  The address we store
	 *  is the address of the printbuf in VME A32 space.
	 */
	hp = &hotrod[c->dev];
	dp = hp->addr;
	dp->mem[256*1024/sizeof(ulong)] = (((ulong)&hp->pbuf) - KZERO) | (SLAVE<<28);
#endif

	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
hotrodcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
hotrodclose(Chan *c)
{
}

/*
 *  read the hotrod memory
 */
long	 
hotrodread(Chan *c, void *buf, long n)
{
	Hotrod *hp;
	Device *dp;
	ulong *from;
	ulong *to;
	ulong *end;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, 0, 0, hotrodgen);

	/*
	 *  allow full word access only
	 */
	if((c->offset&(sizeof(ulong)-1)) || (n&(sizeof(ulong)-1)))
		error(Ebadarg);

	hp = &hotrod[c->dev];
	dp = hp->addr;
	if(c->offset >= sizeof(dp->mem))
		return 0;
	if(c->offset+n > sizeof(dp->mem))
		n = sizeof(dp->mem) - c->offset;

	/*
	 *  avoid memcpy to ensure VME 32-bit reads
	 */
	qlock(hp);
	to = buf;
	from = &dp->mem[c->offset/sizeof(ulong)];
	end = to + (n/sizeof(ulong));
	while(to != end){
		*to++ = *from++;
	}
	qunlock(hp);
	return n;
}

/*
 *  write hotrod memory
 */
long	 
hotrodwrite(Chan *c, void *buf, long n)
{
	Hotrod *hp;
	Device *dp;
	ulong *from;
	ulong *to;
	ulong *end;

	/*
	 *  allow full word access only
	 */
	if((c->offset&(sizeof(ulong)-1)) || (n&(sizeof(ulong)-1)))
		error(Ebadarg);

	hp = &hotrod[c->dev];
	dp = hp->addr;
	if(c->offset >= sizeof(dp->mem))
		return 0;
	if(c->offset+n > sizeof(dp->mem))
		n = sizeof(dp->mem) - c->offset;

	/*
	 *  avoid memcpy to ensure VME 32-bit writes
	 */
	qlock(hp);
	from = buf;
	to = &dp->mem[c->offset/sizeof(ulong)];
	end = to + (n/sizeof(ulong));
	while(to != end)
		*to++ = *from++;
	qunlock(hp);
	return n;
}

void	 
hotrodremove(Chan *c)
{
	error(Eperm);
}

void	 
hotrodwstat(Chan *c, char *dp)
{
	error(Eperm);
}

void
hotrodintr(int vec)
{
	Hotrod *hp;

	print("hotrod%d interrupt\n", vec - Vmevec);
	hp = &hotrod[vec - Vmevec];
	if(hp < hotrod || hp > &hotrod[Nhotrod]){
		print("bad hotrod vec\n");
		return;
	}
}

/*
 *  print hotrod processor messages on the console
 */
void
hotrodkproc(void *a)
{
	Hotrod	*hp = a;
	char	*p;

	hp->kprocstarted = 1;
	hp->pbuf.rptr = hp->pbuf.wptr = hp->pbuf.buf;
	hp->pbuf.lim = &hp->pbuf.buf[sizeof(hp->pbuf.buf)];

	for(;;){
		p = hp->pbuf.wptr;
		if(p != hp->pbuf.rptr){
			if(p > hp->pbuf.rptr){
				putstrn(hp->pbuf.rptr, p - hp->pbuf.rptr);
			} else {
				putstrn(hp->pbuf.rptr, hp->pbuf.lim - hp->pbuf.rptr);
				putstrn(hp->pbuf.buf, hp->pbuf.wptr - hp->pbuf.buf);
			}
			hp->pbuf.rptr = p;
		}
		tsleep(&(hp->r), return0, 0, 1000);
	}
}
