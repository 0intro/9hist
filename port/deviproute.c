#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

typedef	struct Iproute	Iproute;
typedef	struct Iprtab	Iprtab;

enum
{
	Nroutes=	256,
};

/*
 *  Standard ip masks for the 3 classes
 */
uchar classmask[4][4] = {
	0xff,	0,	0,	0,
	0xff,	0,	0,	0,
	0xff,	0xff,	0,	0,
	0xff,	0xff,	0xff,	0,
};
#define CLASSMASK(x)	classmask[(*x>>6) & 3]

uchar netbytes[4] = { 1, 1, 2, 3 };
#define NETBYTES(x)	netbytes[(*x>>6) & 3]

/*
 *  routes
 */
struct Iproute {
	uchar	dst[4];
	uchar	gate[4];
	uchar	mask[4];
	Iproute	*next;
	int	inuse;
};
struct Iprtab {
	Lock;
	int	n;		/* number of valid routes */
	Iproute *first;		/* list of valid routes */
	Iproute	r[Nroutes];	/* all routes */
};
Iprtab	iprtab;

/*
 *  Convert string to ip address.  This is rediculously difficult because
 *  the designers of ip decided to allow any leading zero bytes in the
 *  host part to be left out.
 */
void
strtoip(char *s, uchar *addr)
{
	int i, off, first;
	char *rptr = s;

	/* convert the bytes */
	for(i = 0; i<4 & *rptr; i++)
		addr[i] = strtoul(rptr, &rptr, 0);

	/* move host bytes to the right place */
	first = NETBYTES(addr);
	off = 4 - i;
	if(off)
		while(i != first){
			--i;
			addr[i+off] = addr[i];
		}
}

/*
 *  The chosen route is the one obeys the constraint
 *		r->mask[x] & dst[x] == r->dst[x] for x in 0 1 2 3
 *
 *  If there are several matches, the one whose mask has the most
 *  leading ones (and hence is the most specific) wins.
 *
 *  If there is no match, the default gateway is chosen.
 */
void
iproute(uchar *dst, uchar *gate)
{
	Iproute *r;

	/*
	 *  first check routes
	 */
	lock(&iprtab);
	for(r = iprtab.first; r; r = r->next){
		if((r->mask[0]&dst[0]) == r->dst[0]
		&& (r->mask[1]&dst[1]) == r->dst[1]
		&& (r->mask[2]&dst[2]) == r->dst[2]
		&& (r->mask[3]&dst[3]) == r->dst[3]){
			memmove(gate, r->gate, 4);
			unlock(&iprtab);
			return;
		}
	}
	unlock(&iprtab);

	/*
	 *  else just return what we got
	 */	
	memmove(gate, dst, 4);
}

/*
 *  Compares 2 subnet masks and returns an integer less than, equal to,
 *  or greater than 0, according as m1 is numericly less than,
 *  equal to, or greater than m2.
 */
ipmaskcmp(uchar *m1, uchar *m2)
{
	int a, i;

	for(i = 0; i < 4; i++){
		if(a = *m1++ - *m2++)
			return a;
	}
	return 0;
}

/*
 *  Add a route, create a mask if the first mask is 0.
 *
 *  All routes are stored sorted by the length of leading
 *  ones in the mask.
 *
 *  NOTE: A default route has an all zeroes mask and dst.
 */
void
ipaddroute(uchar *dst, uchar *mask, uchar *gate)
{
	Iproute *r, *e, *free;
	int i;

	if(mask==0)
		mask = CLASSMASK(dst);

	/*
	 *  filter out impossible requests
	 */
	for(i = 0; i < 4; i++)
		if((dst[i]&mask[i]) != dst[i])
			errors("bad ip route");

	/*
	 *  see if we already have a route for
	 *  the destination
	 */
	lock(&iprtab);
	free = 0;
	for(r = iprtab.r; r < &iprtab.r[Nroutes]; r++){
		if(r->inuse == 0){
			free = r;
			continue;
		}
		if(memcmp(dst, r->dst, 4)==0 && memcmp(mask, r->mask, 4)==0){
			memmove(r->gate, gate, 4);
			unlock(&iprtab);
			return;
		}
	}
	if(free == 0)
		errors("no free ip routes");

	/*
	 *  add the new route in sorted order
	 */
	memmove(free->dst, dst, 4);
	memmove(free->mask, mask, 4);
	memmove(free->gate, gate, 4);
	free->inuse = 1;
	for(r = iprtab.first; r; r = r->next){
		if(ipmaskcmp(free->mask, r->mask) > 0)
			break;
		e = r;
	}
	free->next = r;
	if(r == iprtab.first)
		iprtab.first = free;
	else
		e->next = free;
	iprtab.n++;
	unlock(&iprtab);
}

/*
 *  remove a route
 */
void
ipremroute(uchar *dst, uchar *mask)
{
	Iproute *r, *e;

	lock(&iprtab);
	for(r = iprtab.first; r; r = r->next){
		if(memcmp(dst, r->dst, 4)==0 && memcmp(mask, r->mask, 4)==0){
			if(r == iprtab.first)
				iprtab.first = r->next;
			else
				e->next = r->next;
			r->inuse = 0;
			iprtab.n--;
			break;
		}
		e = r;
	}
	unlock(&iprtab);
}

/*
 *  remove all routes
 */
void
ipflushroute(void)
{
	Iproute *r;

	lock(&iprtab);
	for(r = iprtab.first; r; r = r->next)
		r->inuse = 0;
	iprtab.first = 0;
	iprtab.n = 0;
	unlock(&iprtab);
}

/*
 *  device interface
 */
enum{
	Qdir,
	Qdata,
};
Dirtab iproutetab[]={
	"iproute",		Qdata,		0,	0600,
};
#define Niproutetab (sizeof(iproutetab)/sizeof(Dirtab))

void
iproutereset(void)
{
}

void
iprouteinit(void)
{
}

Chan *
iprouteattach(char *spec)
{
	return devattach('R', spec);
}

Chan *
iprouteclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
iproutewalk(Chan *c, char *name)
{
	return devwalk(c, name, iproutetab, (long)Niproutetab, devgen);
}

void
iproutestat(Chan *c, char *db)
{
	devstat(c, db, iproutetab, (long)Niproutetab, devgen);
}

Chan *
iprouteopen(Chan *c, int omode)
{
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
iproutecreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
iprouteremove(Chan *c)
{
	error(Eperm);
}

void
iproutewstat(Chan *c, char *dp)
{
	error(Eperm);
}

void
iprouteclose(Chan *c)
{
}

#define IPR_ENTRYLEN 54
#define PAD "                                                                  "

long
iprouteread(Chan *c, void *a, long n)
{
	char	buf[IPR_ENTRYLEN*2];
	Iproute	*r;
	int	part, bytes, size;

	switch((int)(c->qid.path&~CHDIR)){
	case Qdir:
		return devdirread(c, a, n, iproutetab, Niproutetab, devgen);
	case Qdata:
		lock(&iprtab);
		part = c->offset/IPR_ENTRYLEN;
		for(r = iprtab.first; part && r; r = r->next)
			;
		bytes = c->offset;
		while(bytes < iprtab.n*IPR_ENTRYLEN && n){
			part = bytes%IPR_ENTRYLEN;

			sprint(buf,"%d.%d.%d.%d & %d.%d.%d.%d -> %d.%d.%d.%d%s",
				r->dst[0], r->dst[1], r->dst[2], r->dst[3],
				r->mask[0], r->mask[1], r->mask[2], r->mask[3],
				r->gate[0], r->gate[1], r->gate[2], r->gate[3],
				PAD); 
			
			buf[IPR_ENTRYLEN-1] = '\n';

			size = IPR_ENTRYLEN - part;
			size = MIN(n, size);
			memmove(a, buf+part, size);

			a = (void *)((int)a + size);
			n -= size;
			bytes += size;
		}
		unlock(&iprtab);
		return bytes - c->offset;
		break;
	default:
		n=0;
		break;
	}
	return n;
}

long
iproutewrite(Chan *c, char *a, long n)
{
	char buf[IPR_ENTRYLEN];
	char *field[4];
	uchar mask[4], dst[4], gate[4];
	int m;

	switch((int)(c->qid.path&~CHDIR)){
	case Qdata:
		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 4, ' ');

		if(strncmp(field[0], "flush", 5) == 0)
			ipflushroute();
		else if(strcmp(field[0], "add") == 0){
			switch(m){
			case 4:
				strtoip(field[1], dst);
				strtoip(field[2], mask);
				strtoip(field[3], gate);
				ipaddroute(dst, mask, gate);
				break;
			case 3:
				strtoip(field[1], dst);
				strtoip(field[2], gate);
				ipaddroute(dst, 0, gate);
				break;
			default:
				error(Ebadarg);
			}
		} else if(strcmp(field[0], "delete") == 0){
			switch(m){
			case 3:
				strtoip(field[1], dst);
				strtoip(field[2], mask);
				ipremroute(dst, mask);
				break;
			case 2:
				strtoip(field[1], dst);
				ipremroute(dst, 0);
				break;
			default:
				error(Ebadarg);
			}
		}
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}
