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

void	mem(Hot*, ulong*, ulong);

/*
 * If set, causes data transfers to have checksums
 */
#define	ENABCKSUM	1

typedef struct Hotrod	Hotrod;
typedef struct HotQ	HotQ;

enum {
	Vmevec=		0xd2,		/* vme vector for interrupts */
	Intlevel=	5,		/* level to interrupt on */
	Qdir=		0,		/* Qid's */
	Qhotrod=	1,
	NhotQ=		NRQ,		/* size of communication queues */
	Nhotrod=	1,
};

struct HotQ{
	ulong	i;			/* index into queue */
	Hotmsg	*msg[NhotQ];		/* pointer to command buffer */
	ulong	pad[3];			/* unused; for hotrod prints */
};


struct Hotrod{
	QLock;
	QLock		buflock;
	Lock		busy;
	Hot		*addr;		/* address of the device */
	int		vec;		/* vme interrupt vector */
	HotQ		*wq;		/* write this queue to send cmds */
	int		wi;		/* where to write next cmd */
	HotQ		rq;		/* read this queue to receive replies */
	int		ri;		/* where to read next response */
	Rendez		r;
	uchar		buf[MAXFDATA+MAXMSG];
};

Hotrod hotrod[Nhotrod];

void	hotrodintr(int);

#define	HOTROD	VMEA24SUP(Hot, 0xB00000);

/*
 * Commands
 */
enum{
	RESET=	0,	/* params: Q address, length of queue */
	REBOOT=	1,	/* params: none */
	READ=	2,	/* params: buffer, count, sum, returned count */
	WRITE=	3,	/* params: buffer, count, sum */
	TEST=	7,	/* params: none */
};

void
hotsend(Hotrod *h, Hotmsg *m)
{
/* print("hotsend send %d %d %lux %lux\n", h->wi, m->cmd, m, m->param[0]); /**/
	h->wq->msg[h->wi] = (Hotmsg*)MP2VME(m);
	do
		delay(1);
	while(h->wq->msg[h->wi]);
/* print("hotsend done\n"); /**/
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
		hp->wq = (HotQ*)(&hp->addr->hostrp);
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

#define	NTESTBUF	256
ulong	testbuf[NTESTBUF];

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
		memset(hp->wq->msg, 0, sizeof(hp->wq->msg));
		hp->wq->i = 0;

		/*
		 * Issue reset
		 */
		hp->wi = 0;
		hp->ri = 0;
		mp = &u->khot;
		mp->cmd = RESET;
		mp->param[0] = MP2VME(&hp->rq);
		mp->param[1] = NhotQ;
		hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
		delay(100);
		print("reset\n");

#ifdef asdf
		/*
		 * Issue test
		 */
		mp = &u->khot;
		mp->cmd = TEST;
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
	ulong l, m;

	hp = &hotrod[c->dev];
	switch(c->qid.path){
	case Qhotrod:
		if(n > sizeof hp->buf){
			print("hotrod bufsize\n");
			error(Egreg);
		}
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			/*
			 *  use supplied buffer, no need to lock for reply
			 */
			mp = &u->khot;
			mp->param[2] = 0;	/* checksum */
			mp->param[3] = 0;	/* reply count */
			qlock(hp);
			mp->cmd = READ;
			mp->param[0] = MP2VME(buf);
			mp->param[1] = n;
			hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
			qunlock(hp);
			l = 100*1000*1000;
			do
				m = mp->param[3];
			while(m==0 && --l>0);
			if(m==0 || m>n){
				print("devhotrod: count %ld %ld\n", m, n);
				error(Egreg);
			}
			if(mp->param[2] != hotsum(buf, m, mp->param[2])){
				print("hotrod cksum err is %lux sb %lux\n",
					hotsum(buf, n, 1), mp->param[2]);
				error(Eio);
			}
		}else{
			/*
			 *  use hotrod buffer.  lock the buffer until the reply
			 */
			mp = &u->uhot;
			mp->param[2] = 0;	/* checksum */
			mp->param[3] = 0;	/* reply count */
			qlock(&hp->buflock);
			qlock(hp);
			mp->cmd = READ;
			mp->param[0] = MP2VME(hp->buf);
			mp->param[1] = n;
			hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->uhot);
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
				print("hotrod cksum err is %lux sb %lux\n",
					hotsum((ulong*)hp->buf, n, 1), mp->param[2]);
				qunlock(&hp->buflock);
				error(Eio);
			}
			memcpy(buf, hp->buf, m);
			qunlock(&hp->buflock);
		}
		return m;
	}
	print("hotrod read unk\n");
	error(Egreg);
	return 0;
}

/*
 *  write hotrod memory
 */
long	 
hotrodwrite(Chan *c, void *buf, long n)
{
	Hotrod *hp;
	Hotmsg *mp;

	hp = &hotrod[c->dev];
	switch(c->qid.path){
	case 1:
		if(n > sizeof hp->buf){
			print("hotrod write bufsize\n");
			error(Egreg);
		}
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			/*
			 *  use supplied buffer, no need to lock for reply
			 */
			mp = &u->khot;
			qlock(hp);
			mp->cmd = WRITE;
			mp->param[0] = MP2VME(buf);
			mp->param[1] = n;
			mp->param[2] = hotsum(buf, n, ENABCKSUM);
			hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->khot);
			qunlock(hp);
		}else{
			/*
			 *  use hotrod buffer.  lock the buffer till the reply
			 */
			mp = &u->uhot;
			qlock(&hp->buflock);
			qlock(hp);
			memcpy(hp->buf, buf, n);
			mp->cmd = WRITE;
			mp->param[0] = MP2VME(hp->buf);
			mp->param[1] = n;
			mp->param[2] = hotsum((ulong*)hp->buf, n, ENABCKSUM);
			hotsend(hp, &((User*)(u->p->upage->pa|KZERO))->uhot);
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
	Hotrod *hp;

/*	print("hotrod%d interrupt\n", vec - Vmevec); /**/
	hp = &hotrod[vec - Vmevec];
	if(hp < hotrod || hp > &hotrod[Nhotrod]){
		print("bad hotrod vec\n");
		return;
	}
	hp->addr->lcsr3 &= ~INT_VME;
}

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
