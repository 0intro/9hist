#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

typedef struct Hotrod	Hotrod;
typedef struct HotQ	HotQ;
typedef struct Device	Device;

enum {
	Vmevec=		0xd2,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Qdir=		0,		/* Qid's */
	Qhotrod=	1,
	NhotQ=		10,		/* size of communication queues */
	Nhotrod=	1,
};

/*
 *  The hotrod fiber interface responds to 1MB
 *  of either user or supervisor accesses at:
 *  	0x30000000 to 0x300FFFFF  in	A32 space
 *  and	  0xB00000 to   0xBFFFFF  in	A24 space.
 *  The second 0x40000 of this space is on-board SRAM.
 */
struct Device {
	ulong	mem[0x100000/sizeof(ulong)];
};
#define HOTROD		VMEA24SUP(Device, 0xB00000)

struct HotQ{
	ulong	i;			/* index into queue */
	Hotmsg	*msg[NhotQ];		/* pointer to command buffer */
	ulong	pad[3];			/* unused; for hotrod prints */
};


struct Hotrod{
	QLock;
	Lock		busy;
	Device		*addr;		/* address of the device */
	int		vec;		/* vme interrupt vector */
	HotQ		*wq;		/* write this queue to send cmds */
	int		wi;		/* where to write next cmd */
	HotQ		rq;		/* read this queue to receive replies */
	int		ri;		/* where to read next response */
	Rendez		r;
};

Hotrod hotrod[Nhotrod];

void	hotrodintr(int);

/*
 * Commands
 */
enum{
	RESET=	0,	/* params: Q address, length of queue */
	REBOOT=	1,	/* params: none */
};

void
hotsend(Hotrod *h, Hotmsg *m)
{
print("hotsend send %d %lux %lux\n", m->cmd, m, m->param[0]);
	h->wq->msg[h->wi] = m;
	while(h->wq->msg[h->wi])
		;
print("hotsend done\n");
	h->wi++;
	if(h->wi >= NhotQ)
		h->wi = 0;
}

/*
 *  reset all hotrod boards
 */
void
hotrodreset(void)
{
	int i;
	Hotrod *hp;

	for(hp=hotrod,i=0; i<Nhotrod; i++,hp++){
		hp->addr = HOTROD+i;
		/*
		 * Write queue is at end of hotrod memory
		 */
		hp->wq = (HotQ*)((ulong)hp->addr+2*0x40000-sizeof(HotQ));
		hp->vec = Vmevec+i;
		setvmevec(hp->vec, hotrodintr);
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
	if(c->qid.path != CHDIR)
		return 0;
	if(strncmp(name, "hotrod", 6) == 0){
		c->qid.path = Qhotrod;
		return 1;
	}
	return 0;
}

void	 
hotrodstat(Chan *c, char *dp)
{
	print("hotrodstat\n");
	error(Egreg);
}

Chan*
hotrodopen(Chan *c, int omode)
{
	Device *dp;
	Hotrod *hp;

	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}else if(c->qid.path == Qhotrod){
		hp = &hotrod[c->dev];
		if(!canlock(&hp->busy))
			error(Einuse);
		/*
		 * Clear communications region
		 */
		memset(hp->wq->msg, 0, sizeof(hp->wq->msg));
		hp->wq->i = 0;

		/*
		 * Issue reset
		 */
		hp->wi = 0;
		hp->ri = 0;
		u->khot.cmd = RESET;
		u->khot.param[0] = (ulong)&hp->rq;
		u->khot.param[1] = NhotQ;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
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
	Hotrod *hp;

	hp = &hotrod[c->dev];
	if(c->qid.path != CHDIR){
		u->khot.cmd = REBOOT;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
		unlock(&hp->busy);
	}
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

	if(c->qid.path != Qhotrod)
		error(Egreg);

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

	if(c->qid.path != Qhotrod)
		error(Egreg);
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
