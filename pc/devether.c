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
ethercreate(Chan*, char*, int, ulong)
{
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
etherrloop(Ether *ctlr, Etherpkt *pkt, long len)
{
	Block *bp;
	ushort type;
	int i, n;
	Netfile *f, **fp, **ep;

	type = (pkt->type[0]<<8)|pkt->type[1];
	ep = &ctlr->f[Ntypes];
	for(fp = ctlr->f; fp < ep; fp++){
		if((f = *fp) && (f->type == type || f->type < 0)){
			if(f->type > -2){
				if(qproduce(f->in, pkt->d, len) < 0)
					ctlr->soverflows++;
			} else {
				if(qwindow(f->in) <= 0)
					continue;
				if(len > 64)
					n = 64;
				else
					n = len;
				bp = iallocb(n);
				if(bp == 0)
					continue;
				memmove(bp->wp, pkt->d, n);
				i = TK2MS(m->ticks);
				bp->wp[58] = len>>8;
				bp->wp[59] = len;
				bp->wp[60] = i>>24;
				bp->wp[61] = i>>16;
				bp->wp[62] = i>>8;
				bp->wp[63] = i;
				bp->wp += 64;
				qpass(f->in, bp);
			}
		}
	}
}

static int
etherwloop(Ether *ctlr, Etherpkt *pkt, long len)
{
	int s, different;

	different = memcmp(pkt->d, ctlr->ea, sizeof(pkt->d));
	if(!ctlr->prom && different && memcmp(pkt->d, ctlr->bcast, sizeof(pkt->d)))
		return 0;

	s = splhi();
	etherrloop(ctlr, pkt, len);
	splx(s);

	return different == 0;
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

			print("ether%d: %s: port 0x%luX irq %d",
				ctlrno, ctlr->type, ctlr->port, ctlr->irq);
			if(ctlr->mem)
				print(" addr 0x%luX", ctlr->mem & ~KZERO);
			if(ctlr->size)
				print(" size 0x%luX", ctlr->size);
			print(":");
			for(i = 0; i < sizeof(ctlr->ea); i++)
				print(" %2.2uX", ctlr->ea[i]);
			print("\n");

			netifinit(ctlr, "ether", Ntypes, 32*1024);
			ctlr->alen = Eaddrlen;
			memmove(ctlr->addr, ctlr->ea, sizeof(ctlr->ea));
			memmove(ctlr->bcast, etherbcast, sizeof(etherbcast));

			ctlr->ctlrno = ctlrno;
			ether[ctlrno] = ctlr;
			ctlr = 0;
			break;
		}
	}
	if(ctlr)
		free(ctlr);
}
