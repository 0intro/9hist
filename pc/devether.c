#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"
#include "../port/netif.h"

#include "etherif.h"

static Ether *ether[MaxEther];

void
etherinit(void)
{
}

Chan*
etherattach(char *spec)
{
	ulong ctlrno;
	char *p;
	Chan *c;

	ctlrno = 0;
	if(spec && *spec){
		ctlrno = strtoul(spec, &p, 0);
		if((ctlrno == 0 && p == spec) || *p || (ctlrno >= MaxEther))
			error(Ebadarg);
	}
	if(ether[ctlrno] == 0)
		error(Enodev);

	c = devattach('l', spec);
	c->dev = ctlrno;
	if(ether[ctlrno]->attach)
		(*ether[ctlrno]->attach)(ether[ctlrno]);
	return c;
}

Chan*
etherclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
etherwalk(Chan *c, char *name)
{
	return netifwalk(ether[c->dev], c, name);
}

void
etherstat(Chan *c, char *dp)
{
	netifstat(ether[c->dev], c, dp);
}

Chan*
etheropen(Chan *c, int omode)
{
	return netifopen(ether[c->dev], c, omode);
}

void
ethercreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
}

void
etherclose(Chan *c)
{
	netifclose(ether[c->dev], c);
}

long
etherread(Chan *c, void *buf, long n, ulong offset)
{
	return netifread(ether[c->dev], c, buf, n, offset);
}

Block*
etherbread(Chan *c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

void
etherremove(Chan *c)
{
	USED(c);
}

void
etherwstat(Chan *c, char *dp)
{
	netifwstat(ether[c->dev], c, dp);
}

void
etherrloop(Ether *ctlr, Etherpkt *pkt, long len, int tome)
{
	ushort type;
	Netfile *f, **fp, **ep;

	type = (pkt->type[0]<<8)|pkt->type[1];
	ep = &ctlr->f[Ntypes];
	for(fp = ctlr->f; fp < ep; fp++){
		if((f = *fp) && ((tome && f->type==type) || f->type < 0))
			qproduce(f->in, pkt->d, len);
	}
}

static int
etherwloop(Ether *ctlr, Etherpkt *pkt, long len)
{
	int s, tome, bcast;

	tome = memcmp(pkt->d, ctlr->ea, sizeof(pkt->d)) == 0;
	bcast = memcmp(pkt->d, ctlr->bcast, sizeof(pkt->d)) == 0;
	if(!tome && !bcast && ctlr->prom==0 && ctlr->all==0)
		return 0;

	s = splhi();
	etherrloop(ctlr, pkt, len, tome||bcast);
	splx(s);

	return tome;
}

long
etherwrite(Chan *c, void *buf, long n, ulong offset)
{
	Ether *ctlr;

	USED(offset);
	if(n > ETHERMAXTU)
		error(Ebadarg);
	ctlr = ether[c->dev];

	if(NETTYPE(c->qid.path) != Ndataqid)
		return netifwrite(ctlr, c, buf, n);

	if(etherwloop(ctlr, buf, n))
		return n;

	qlock(&ctlr->tlock);
	if(waserror()){
		qunlock(&ctlr->tlock);
		nexterror();
	}
	n = (*ctlr->write)(ctlr, buf, n);
	poperror();
	qunlock(&ctlr->tlock);

	return n;
}

long
etherbwrite(Chan *c, Block *bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

static struct {
	char	*type;
	int	(*reset)(Ether*);
} cards[MaxEther+1];

void
addethercard(char *t, int (*r)(Ether*))
{
	static int ncard;

	if(ncard == MaxEther)
		panic("too many ether cards");
	cards[ncard].type = t;
	cards[ncard].reset = r;
	ncard++;
}

void
etherreset(void)
{
	Ether *ctlr;
	int i, n, ctlrno;

	for(ctlr = 0, ctlrno = 0; ctlrno < MaxEther; ctlrno++){
		if(ctlr == 0)
			ctlr = malloc(sizeof(Ether));
		memset(ctlr, 0, sizeof(Ether));
		if(isaconfig("ether", ctlrno, ctlr) == 0)
			continue;
		for(n = 0; cards[n].type; n++){
			if(strcmp(cards[n].type, ctlr->type))
				continue;
			if((*cards[n].reset)(ctlr))
				break;

			/*
			 * IRQ2 doesn't really exist, it's used to gang the interrupt
			 * controllers together. A device set to IRQ2 will appear on
			 * the second interrupt controller as IRQ9.
			 */
			if(ctlr->irq == 2)
				ctlr->irq = 9;
			setvec(Int0vec+ctlr->irq, ctlr->interrupt, ctlr);

			print("ether%d: %s: port 0x%lux irq %d",
				ctlrno, ctlr->type, ctlr->port, ctlr->irq);
			if(ctlr->mem)
				print(" addr 0x%lux", ctlr->mem & ~KZERO);
			if(ctlr->size)
				print(" size 0x%lux", ctlr->size);
			print(":");
			for(i = 0; i < sizeof(ctlr->ea); i++)
				print(" %2.2ux", ctlr->ea[i]);
			print("\n");

			netifinit(ctlr, "ether", Ntypes, 32*1024);
			ctlr->alen = Eaddrlen;
			memmove(ctlr->addr, ctlr->ea, sizeof(ctlr->ea));
			memmove(ctlr->bcast, etherbcast, sizeof(etherbcast));

			ether[ctlrno] = ctlr;
			ctlr = 0;
			break;
		}
	}
	if(ctlr)
		free(ctlr);
}
