#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"arp.h"
#include 	"ipdat.h"

#include	"devtab.h"

Arpstats	arpstats;
Arpcache 	*arplruhead, *arplrutail;
Arpcache 	*arp, **arphash;
Queue		*Servq;

typedef struct Arpq Arpq;
struct Arpq
{
	uchar	ip[4];
	uchar	*etheraddr;
	Block	*bp;
	Queue	*put;
	ulong	time;
	Arpq	*next;
};

struct arpalloc
{
	Lock;
	Lock	list;
	Lock	hash;
	Arpq	*free;
	Arpq	*head;
	Arpq	*tail;
}arpalloc;

void		arpiput(Queue *, Block *);
void		arpoput(Queue *, Block *);
void		arpopn(Queue *, Stream *);
void		arpcls(Queue *);
Qinfo arpinfo = { arpiput, arpoput, arpopn, arpcls, "arp" };

#define ARP_ENTRYLEN	50
char *padstr = "                                           ";

enum{
	arpdirqid,
	arpstatqid,
	arpctlqid,
	arpdataqid,
};

Dirtab arptab[]={
	"stats",	{arpstatqid},		0,	0600,
	"ctl",		{arpctlqid},		0,	0600,
	"data",		{arpdataqid},		0,	0600,
};
#define Narptab (sizeof(arptab)/sizeof(Dirtab))

void
arpreset(void)
{
	Arpcache *ap, *ep;

	arp = (Arpcache *)ialloc(sizeof(Arpcache) * conf.arp, 0);
	arphash = (Arpcache **)ialloc(sizeof(Arpcache *) * Arphashsize, 0);

	ep = &arp[conf.arp];
	for(ap = arp; ap < ep; ap++) {
		ap->frwd = ap+1;
		ap->prev = ap-1;
		ap->type = ARP_FREE;
		ap->status = ARP_TEMP;
	}

	arp[0].prev = 0;
	arplruhead = arp;
	ap = &arp[conf.arp-1];
	ap->frwd = 0;
	arplrutail = ap;
	newqinfo(&arpinfo);
}

void
arpinit(void)
{
}

Chan *
arpattach(char *spec)
{
	return devattach('a', spec);
}

Chan *
arpclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
arpwalk(Chan *c, char *name)
{
	return devwalk(c, name, arptab, (long)Narptab, devgen);
}

void
arpstat(Chan *c, char *db)
{
	devstat(c, db, arptab, (long)Narptab, devgen);
}

Chan *
arpopen(Chan *c, int omode)
{

	if(c->qid.path == CHDIR){
		if(omode != OREAD)
			error(Eperm);
	}

	switch(STREAMTYPE(c->qid.path)) {
	case arpdataqid:
		break;
	case arpstatqid:
		if(omode != OREAD)
			error(Ebadarg);
		break;
	case arpctlqid:
		break;
	}


	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
arpcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
arpremove(Chan *c)
{
	error(Eperm);
}

void
arpwstat(Chan *c, char *dp)
{
	error(Eperm);
}

void
arpclose(Chan *c)
{
	streamclose(c);
}

long
arpread(Chan *c, void *a, long n, ulong offset)
{
	char	 buf[100];
	Arpcache *ap, *ep;
	int	 part, bytes, size;
	char	 *ptr, *ststr;

	switch((int)(c->qid.path&~CHDIR)){
	case arpdirqid:
		return devdirread(c, a, n, arptab, Narptab, devgen);
	case arpdataqid:
		bytes = c->offset;
		while(bytes < conf.arp*ARP_ENTRYLEN && n) {
			ap = &arp[bytes/ARP_ENTRYLEN];
			part = bytes%ARP_ENTRYLEN;

			if(ap->status != ARP_OK)
				ststr = "invalid";
			else
				ststr = (ap->type == ARP_TEMP ? "temp" : "perm");

			sprint(buf,"%d.%d.%d.%d to %.2x:%.2x:%.2x:%.2x:%.2x:%.2x %s%s",
				ap->eip[0], ap->eip[1], ap->eip[2], ap->eip[3],
				ap->et[0], ap->et[1], ap->et[2], ap->et[3],
				ap->et[4], ap->et[5],
				ststr, padstr); 
			
			buf[ARP_ENTRYLEN-1] = '\n';

			size = ARP_ENTRYLEN - part;
			size = MIN(n, size);
			memmove(a, buf+part, size);

			a = (void *)((int)a + size);
			n -= size;
			bytes += size;
		}
		return bytes - c->offset;
		break;
	case arpstatqid:
		sprint(buf, "hits: %d miss: %d failed: %d\n",
			arpstats.hit, arpstats.miss, arpstats.failed);

		return stringread(c, a, n, buf, offset);
	default:
		n=0;
		break;
	}
	return n;
}

long
arpwrite(Chan *c, char *a, long n, ulong offset)
{
	Arpentry entry;
	char	 buf[20], *field[5];
	int 	 m;

	switch(STREAMTYPE(c->qid.path)) {
	case arpctlqid:

		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 5, ' ');

		if(strncmp(field[0], "flush", 5) == 0)
			arpflush();
		else if(strcmp(field[0], "delete") == 0) {
			if(m != 2)
				error(Ebadarg);

			if(arpdelete(field[1]) < 0)
				error(Eaddrnotfound);
		}
	case arpdataqid:
		if(n != sizeof(Arpentry))
			error(Emsgsize);
		memmove(&entry, a, sizeof(Arpentry));
		arpenter(&entry, ARP_TEMP);
		break;
	default:
		error(Ebadusefd);
	}

	return n;
}


void
arpopn(Queue *q, Stream *s)
{
	if(!Servq)
		Servq = RD(q);
}

void
arpcls(Queue *q)
{
	if(q == Servq)
		Servq = 0;
}

void
arpiput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

void
arpoput(Queue *q, Block *bp)
{
	PUTNEXT(q, bp);
}

int
arplookup(uchar *ip, uchar *et)
{
	Arpcache *ap;

	lock(&arpalloc.hash);
	for(ap = ARPHASH(ip); ap; ap = ap->hash) {
		if(ap->status == ARP_OK && memcmp(ap->eip, ip, sizeof(ap->eip)) == 0) {
			memmove(et, ap->et, sizeof(ap->et));
			arplinkhead(ap);
			unlock(&arpalloc.hash);
			arpstats.hit++;
			return 1;
		}
	}
	unlock(&arpalloc.hash);
	return 0;
}

void
arpsendpkt(uchar *unroutedip, uchar *ether, Queue *put, Block *bp)
{
	Arpq *aq;
	Block *nbp;
	uchar ip[4];

	if(!Servq) {
		print("arp: No server\n");
		freeb(bp);
		return;
	}

	iproute(unroutedip, ip);
	if(arplookup(ip, ether)) {
		PUTNEXT(put, bp);
		return;
	}

	/* Send the request out to the user level arp daemon */
	nbp = allocb(sizeof(ip));
	memmove(nbp->rptr, ip, sizeof(ip));
	nbp->wptr += sizeof(ip);
	nbp->flags |= S_DELIM;
	PUTNEXT(Servq, nbp);
	arpstats.miss++;

	lock(&arpalloc);
	if(aq = arpalloc.free)
		arpalloc.free = aq->next;
	unlock(&arpalloc);

	if(aq == 0) {
		freeb(bp);
		return;
	}

	/* Stash the work away until the arp completes or times out */
	memmove(aq->ip, ip, sizeof(aq->ip));
	aq->etheraddr = ether;
	aq->bp = bp;
	aq->put = put;
	aq->time = MACHP(0)->ticks;

	lock(&arpalloc.list);
	if(arpalloc.head)  {
		arpalloc.tail->next = aq;
		arpalloc.tail = aq;
	}
	else {
		arpalloc.tail = aq;
		arpalloc.head = aq;
	}
	aq->next = 0;
	unlock(&arpalloc.list);
}

void
arpflush(void)
{
	Arpcache *ap;

	for(ap = arplruhead; ap; ap = ap->frwd)
		ap->status = ARP_FREE;
}

void
arpenter(Arpentry *ape, int type)
{
	Arpcache *ap, **l, *d;


	/* Update an entry if we have one already */
	l = &ARPHASH(ape->ipaddr);
	lock(&arpalloc.hash);
	for(ap = *l; ap; ap = ap->hash) {
		if(ap->status == ARP_OK && memcmp(ap->eip, ape->ipaddr, sizeof(ap->eip)) == 0) {
			if(ap->type != ARP_PERM) {
				ap->type = type;
				memmove(ap->et, ape->etaddr, sizeof(ap->et));
				ap->status = ARP_OK;
			}
			unlock(&arpalloc.hash);
			return;
		}
	}

	/* Find an entry to replace */
	for(ap = arplrutail; ap && ap->type == ARP_PERM; ap = ap->prev)
		;

	if(!ap) {
		print("arp: too many permanent entries\n");
		unlock(&arpalloc.hash);
		return;
	}

	if(ap->hashhd) {
		for(d = *ap->hashhd; d; d = d->hash) {
			if(d == ap) {
				*(ap->hashhd) = ap->hash;
				break;
			}
			ap->hashhd = &d->hash;
		}
	}

	ap->type = type;
	ap->status = ARP_OK;
	memmove(ap->eip, ape->ipaddr, sizeof(ape->ipaddr));
	memmove(ap->et, ape->etaddr, sizeof(ape->etaddr));
	ap->ip = nhgetl(ap->eip);
	ap->hashhd = l;
	ap->hash = *l;
	*l = ap;
	arplinkhead(ap);
	unlock(&arpalloc.hash);
	pusharpq();
}

void
pusharpq(void)
{
	int sent;
	Arpq *aq, *prev;

loop:	prev = 0;
	lock(&arpalloc.list);
	for(aq = arpalloc.head; aq; aq = aq->next) {
		if(arplookup(aq->ip, aq->etheraddr)) {
			if(prev)
				prev->next = aq->next;
			else
				arpalloc.head = 0;
			if(aq->next == 0)
				arpalloc.tail = prev;
			unlock(&arpalloc.list);
			PUTNEXT(aq->put, aq->bp);

			lock(&arpalloc);
			aq->next = arpalloc.free;
			arpalloc.free = aq;
			unlock(&arpalloc);
			goto loop;
		}
		prev = aq;
	}
	unlock(&arpalloc.list);
}

int
arpdelete(char *addr)
{
	Arpcache *ap;
	char enetaddr[6], buf[20], *ptr;
	int i;

	ptr = buf + 2;
	strncpy(ptr, addr, (sizeof buf) - 2);

	for(i = 0; i < 6 && addr != (char *)1; i++) {
		ptr[-2] = '0';
		ptr[-1] = 'x';
		enetaddr[i] = atoi(ptr-2);
		ptr = strchr(ptr, ':')+1;
	}

	lock(&arpalloc.hash);
	for(ap = arplruhead; ap; ap = ap->frwd) {
		if(memcmp(ap->et, ptr, sizeof(ap->et)) == 0) {
			ap->status = ARP_FREE;
			break;
		}
	}
	unlock(&arpalloc.hash);
}

void
arplinkhead(Arpcache *ap)
{
	if(ap != arplruhead) {
		if(ap->prev)
			ap->prev->frwd = ap->frwd;
		else
			arplruhead = ap->frwd;
	
		if(ap->frwd)
			ap->frwd->prev = ap->prev;
		else
			arplrutail = ap->prev;
		
		ap->frwd = arplruhead;
		ap->prev = 0;
		arplruhead = ap;
	}
}
