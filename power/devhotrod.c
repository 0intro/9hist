#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"fcall.h"

#include	"io.h"
#include	"hrod.h"

/*
 * If defined, ENABMEMTEST causes memory test to be run at device open
 */
#ifdef	ENABMEMTEST
void	mem(Hot*, ulong*, ulong);
#define	NTESTBUF	256
ulong	testbuf[NTESTBUF];
#endif
/*
 * If defined, ENABBUSTEST causes bus error diagnostic to be run at device open
 */

/*
 * If 1, ENABCKSUM causes data transfers to have checksums
 */
#define	ENABCKSUM	1

typedef struct Hotrod	Hotrod;

enum{
	Vmevec=		0xd2,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Qdir=		0,		/* Qid's */
	Qhotrod=	1,
	Nhotrod=	1,
};

Dirtab hotroddir[]={
	"hotrod",	{Qhotrod},	0,	0600,
};

#define	NHOTRODDIR	(sizeof hotroddir/sizeof(Dirtab))

struct Hotrod{
	QLock;
	QLock		buflock;
	Lock		busy;
	Hot		*addr;		/* address of the device */
	int		vec;		/* vme interrupt vector */
	int		wi;		/* where to write next cmd */
	int		ri;		/* where to read next reply */
	uchar		buf[MAXFDATA+MAXMSG];
};

Hotrod hotrod[Nhotrod];

void	hotrodintr(int);

#define	HOTROD	VMEA24SUP(Hot, 0xB00000);

/*
 * Commands
 */
void
hotsend(Hotrod *h, Hotmsg *m)
{
	Hotmsg **mp;
	long l;

/* print("hotsend send %d %d %lux %lux\n", h->wi, m->cmd, m, m->param[0]); /**/
	mp = (Hotmsg**)&h->addr->reqstq[h->wi];
	*mp = (Hotmsg*)MP2VME(m);
	h->wi++;
	if(h->wi >= NRQ)
		h->wi = 0;
	l = 0;
	while(*mp){
		delay(0);	/* just a subroutine call; stay off VME */
		if(++l > 1000*1000){
			l = 0;
			print("hotsend blocked\n");
		}
	}
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
		hp->vec = Vmevec+i;
		setvmevec(hp->vec, hotrodintr);
	}	
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
	return devwalk(c, name, hotroddir, NHOTRODDIR, devgen);
}

void	 
hotrodstat(Chan *c, char *dp)
{
	devstat(c, dp, hotroddir, NHOTRODDIR, devgen);
}

Chan*
hotrodopen(Chan *c, int omode)
{
	Hotrod *hp;
	Hotmsg *mp;

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
		memset(hp->addr->reqstq, 0, NRQ*sizeof(ulong));
		hp->addr->reqstp = 0;
		memset(hp->addr->replyq, 0, NRQ*sizeof(ulong));
		hp->addr->replyp = 0;

		/*
		 * Issue reset
		 */
		hp->wi = 0;
		hp->ri = 0;
		mp = &u->khot;
		mp->cmd = Ureset;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
		delay(100);
		print("reset\n");

#ifdef ENABMEMTEST
		/*
		 * Issue test
		 */
		mp = &u->khot;
		mp->cmd = Utest;
		mp->param[0] = MP2VME(testbuf);
		mp->param[1] = NTESTBUF;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
		delay(100);
		print("testing addr %lux size %ld\n", mp->param[0], mp->param[1]);
		for(;;){
			print("-");
			mem(hp->addr, &hp->addr->ram[(mp->param[0]-0x40000)/sizeof(ulong)], mp->param[1]);
		}
#endif
#ifdef ENABBUSTEST
		/*
		 * Issue test
		 */
		mp = &u->khot;
		mp->cmd = Ubus;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
#endif
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
		u->khot.cmd = Ureboot;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
		unlock(&hp->busy);
	}
}

ulong
hotsum(ulong *p, int n, ulong doit)
{
	ulong sum;

	if(!doit)
		return 0;
	sum = 0;
	n = (n+sizeof(ulong)-1)/sizeof(ulong);
	while(--n >= 0)
		sum += *p++;
	return sum;
}

int
hotmsgintr(Hotmsg *hm)
{
	return hm->intr;
}

/*
 * Read and write use physical addresses if they can, which they usually can.
 * Most I/O is from devmnt, which has local buffers.  Therefore just check
 * that buf is in KSEG0 and is at an even address.
 */

long	 
hotrodread(Chan *c, void *buf, long n)
{
	Hotrod *hp;
	Hotmsg *mp;
	ulong l, m, isflush;

	hp = &hotrod[c->dev];
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, hotroddir, NHOTRODDIR, devgen);

	case Qhotrod:
		if(n > sizeof hp->buf){
			print("hotrod bufsize\n");
			error(Egreg);
		}
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			/*
			 *  use supplied buffer, no need to lock for reply
			 */
			isflush = 0;
memset(buf, 0, n);
			mp = &((User*)(u->p->upage->pa|KZERO))->khot;
			if(mp->abort){	/* use reserved flush msg */
				mp = &((User*)(u->p->upage->pa|KZERO))->fhot;
				isflush = 1;
			}
			mp->param[2] = 0;	/* reply checksum */
			mp->param[3] = 0;	/* reply count */
			qlock(hp);
			mp->cmd = Uread;
			mp->param[0] = MP2VME(buf);
			mp->param[1] = n;
			mp->abort = isflush;
			mp->intr = 0;
			hotsend(hp, mp);
			qunlock(hp);
			if(isflush){		/* busy loop */
				l = 100*1000*1000;
				do
					m = mp->param[3];
				while(m==0 && --l>0);
			}else{
				if(waserror()){
					mp->abort = 1;
					nexterror();
				}
				sleep(&mp->r, hotmsgintr, mp);
				m = mp->param[3];
			}
			if(m==0 || m>n){
				print("devhotrod: count %ld %ld\n", m, n);
				error(Egreg);
			}
			if(mp->param[2] != hotsum(buf, m, mp->param[2])){
				hp->addr->error++;
				print("hotrod cksum err is %lux sb %lux\n",
					hotsum(buf, m, 1), mp->param[2]);
				print("addr %lux\n", ((char*)buf)+m);
				{
					int i;
					ulong *p = buf;
					for(i=2; i<m/4; i++)
						if(p[i] != i-2)
							print("%d sb %d %lux %lux\n", p[i], i-2, p[i], &p[i]);
				}
				error(Eio);
			}
			mp->abort = 0;
			if(isflush)
				u->khot.abort = 0;	/* flushed message's done too */
			else
				poperror();
		}else{
			/*
			 * use hotrod buffer. lock the buffer until the reply
			 */
			mp = &((User*)(u->p->upage->pa|KZERO))->uhot;
			mp->param[2] = 0;	/* reply checksum */
			mp->param[3] = 0;	/* reply count */
			qlock(&hp->buflock);
			qlock(hp);
			mp->cmd = Uread;
			mp->param[0] = MP2VME(hp->buf);
			mp->param[1] = n;
			mp->abort = 1;
			mp->intr = 0;
			hotsend(hp, mp);
			qunlock(hp);
			l = 100*1000*1000;
			do
				m = mp->param[3];
			while(m==0 && --l>0);
			if(m==0 || m>n){
				print("devhotrod: count %ld %ld\n", m, n);
				qunlock(&hp->buflock);
				error(Egreg);
			}
			if(mp->param[2] != hotsum((ulong*)hp->buf, m, mp->param[2])){
				hp->addr->error++;
				print("hotrod cksum err is %lux sb %lux\n",
					hotsum((ulong*)hp->buf, m, 1), mp->param[2]);
				qunlock(&hp->buflock);
{int i; for(i=0; i<8; i++) print("%lux\n", ((ulong*)buf)[i]); }
				error(Eio);
			}
			memmove(buf, hp->buf, m);
			mp->abort = 0;
			qunlock(&hp->buflock);
		}
		return m;
	}
	print("hotrod read unk\n");
	error(Egreg);
	return 0;
}

long	 
hotrodwrite(Chan *c, void *buf, long n)
{
	Hotrod *hp;
	Hotmsg *mp;

	hp = &hotrod[c->dev];
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, hotroddir, NHOTRODDIR, devgen);

	case Qhotrod:
		if(n > sizeof hp->buf){
			print("hotrod write bufsize\n");
			error(Egreg);
		}
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			/*
			 * use supplied buffer, no need to lock for reply
			 */
			mp = &((User*)(u->p->upage->pa|KZERO))->khot;
			mp->wtype = ((char*)buf)[0];
			if(mp->abort)	/* use reserved flush msg */
				mp = &((User*)(u->p->upage->pa|KZERO))->fhot;
			qlock(hp);
			mp->cmd = Uwrite;
			mp->param[0] = MP2VME(buf);
			mp->param[1] = n;
			mp->param[2] = hotsum(buf, n, ENABCKSUM);
			hotsend(hp, mp);
			qunlock(hp);
		}else{
			/*
			 * use hotrod buffer.  lock the buffer until the reply
			 */
			mp = &((User*)(u->p->upage->pa|KZERO))->uhot;
			mp->wtype = ((char*)buf)[0];
			qlock(&hp->buflock);
			qlock(hp);
			memmove(hp->buf, buf, n);
			mp->cmd = Uwrite;
			mp->param[0] = MP2VME(hp->buf);
			mp->param[1] = n;
			mp->param[2] = hotsum((ulong*)hp->buf, n, ENABCKSUM);
			hotsend(hp, mp);
			qunlock(hp);
			qunlock(&hp->buflock);
		}
		return n;
	}
	print("hotrod write unk\n");
	error(Egreg);
	return 0;
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
	Hotrod *h;
	Hotmsg *hm;
	ulong l;

	h = &hotrod[vec - Vmevec];
	if(h<hotrod || h>&hotrod[Nhotrod]){
		print("bad hotrod vec\n");
		return;
	}
	h->addr->lcsr3 &= ~INT_VME;
	while(l = h->addr->replyq[h->ri]){	/* assign = */
		hm = (Hotmsg*)(VME2MP(l));
		h->addr->replyq[h->ri] = 0;
		h->ri++;
		if(h->ri >= NRQ)
			h->ri = 0;
		hm->intr = 1;
		if(hm->abort)
			print("abort wakeup\n");
		else
			wakeup(&hm->r);
	}
}

#ifdef	ENABMEMTEST
void
mem(Hot *hot, ulong *buf, ulong size)
{
	long i, j, k, l;
	ulong *p, bit, u;
	int q;

goto part4;
	/* one bit */
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		*p = bit;
		bit <<= 1;
	}
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		if(*p != bit) {
			print("A: %lux is %lux sb %lux\n", p, *p, bit);
			hot->error++;
			delay(500);
		}
		bit <<= 1;
	}
	/* all but one bit */
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		*p = ~bit;
		bit <<= 1;
	}
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		if(*p != ~bit) {
			print("B: %lux is %lux sb %lux\n", p, *p, ~bit);
			hot->error++;
			delay(500);
		}
		bit <<= 1;
	}
	/* rand bit */
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		*p = bit;
		bit += PRIME;
	}
	bit = 0;
	p = buf;
	for(i=0; i<size; i++,p++) {
		if(bit == 0)
			bit = 1;
		if(*p != bit) {
			print("C: %lux is %lux sb %lux\n", p, *p, bit);
			hot->error++;
			delay(500);
		}
		bit += PRIME;
	}
part4:
	/* address */
	p = buf;
	for(i=0; i<size; i++,p++)
		*p = i;
	for(j=0; j<200; j++) {
		p = buf;
		for(i=0; i<size; i++,p++) {
			u = *p;
			if(u != i+j) {
				print("D: %lux is %lux sb %lux (%lux)\n", p, u, i+j, *p);
			hot->error++;
				delay(500);
			}
			*p = i+j+1;
		}
	}
}
#endif
