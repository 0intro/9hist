#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"arp.h"
#include 	"ipdat.h"

#include	"devtab.h"

Arpcache 	*arp;
Arpcache	**arphash;
Arpstats	arpstats;
Queue		*Servq;

#define ARP_ENTRYLEN	50
char *padstr = "                                           ";

extern Arpcache *arplruhead;
extern Arpcache *arplrutail;

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
			arp_flush();
		else if(strcmp(field[0], "delete") == 0) {
			if(m != 2)
				error(Ebadarg);

			if(arp_delete(field[1]) < 0)
				error(Eaddrnotfound);
		}
	case arpdataqid:
		if(n != sizeof(Arpentry))
			error(Emsgsize);
		memmove(&entry, a, sizeof(Arpentry));
		arp_enter(&entry, ARP_TEMP);
		break;
	default:
		error(Ebadusefd);
	}

	return n;
}

