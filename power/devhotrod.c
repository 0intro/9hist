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

enum {
	Vmevec=		0xd2,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Nhotrod=	1,
};

/*
 *  the hotrod fiber interface responds to 1MB
 *  of either user or supervisor accesses at:
 *  	0x30000000 to 0x300FFFFF  in	A32 space
 *  and	  0xB00000 to   0xBFFFFF  in	A24 space
 */
struct Device {
	uchar	mem[1*1024*1024];
};
#define HOTROD		VMEA32SUP(Device, 0x30000000)

struct Hotrod {
	QLock;

	Device	*addr;
	int	vec;
	char	name[NAMELEN];
};

Hotrod hotrod[Nhotrod];

static void hotrodintr(int);

int
hotrodgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	if(i || c->dev>=Nhotrod)
		return -1;
	devdir(c, 0, hotrod[c->dev].name, sizeof(Device), 0666, dp);
	return 1;
}

/*
 *  reset all hotrod boards
 */
void
hotrodreset(void)
{
	int i;
	Hsvme *hp;

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
	int i;
	Chan *c;

	i = strtoul(spec, 0, 0);
	if(i >= Nhotrod)
		error(0, Ebadarg);

	c = devattach('H', spec);
	c->dev = i;
	c->qid = CHDIR;
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
	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eperm);
	}else
		streamopen(c, &hotrodinfo);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
hotrodcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
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

	hp = &hotrod[c->dev];
	dp = hp->addr;
	if(c->offset >= sizeof(dp->mem))
		return 0;
	if(c->offset+n > sizeof(dp->mem))
		n = sizeof(dp->mem) - c->offset;
	qlock(hp);
	memcpy(buf, &dp->mem[c->offset], n);
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

	hp = &hotrod[c->dev];
	dp = hp->addr;
	if(c->offset >= sizeof(dp->mem))
		return 0;
	if(c->offset+n > sizeof(dp->mem))
		n = sizeof(dp->mem) - c->offset;
	qlock(hp);
	memcpy(&dp->mem[c->offset], buf, n);
	qunlock(hp);
	return n;
}

void	 
hotrodremove(Chan *c)
{
	error(0, Eperm);
}

void	 
hotrodwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
hotroduserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
hotroderrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

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
