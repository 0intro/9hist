#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

/*
 *  address resolution tables
 */

enum
{
	NHASH		= (1<<6),
	NCACHE		= 256,

	AOK		= 1,
	AWAIT		= 2,
};

char *arpstate[] =
{
	"UNUSED",
	"OK",
	"WAIT",
};

typedef struct Arpcache Arpcache;
struct Arpcache
{
	QLock;
	Arpent*	hash[NHASH];
	Arpent	cache[NCACHE];
};

Arpcache	arp;

char *Ebadarp = "bad arp";

#define haship(s) ((s)[IPaddrlen-1]%NHASH)

static Arpent*
newarp(uchar *ip, Medium *m)
{
	uint t;
	Block *next, *xp;
	Arpent *a, *e, *f, **l;

	/* find oldest entry */
	e = &arp.cache[NCACHE];
	a = arp.cache;
	t = a->used;
	for(f = a; f < e; f++){
		if(f->used < t){
			t = f->used;
			a = f;
		}
	}

	/* dump waiting packets */
	xp = a->hold;
	a->hold = nil;
	while(xp){
		next = xp->list;
		freeblist(xp);
		xp = next;
	}

	/* take out of current chain */
	l = &arp.hash[haship(a->ip)];
	for(f = *l; f; f = f->hash){
		if(f == a) {
			*l = a->hash;
			break;
		}
		l = &f->hash;
	}

	/* insert into new chain */
	l = &arp.hash[haship(ip)];
	a->hash = *l;
	*l = a;
	memmove(a->ip, ip, sizeof(a->ip));
	a->used = msec;
	a->time = 0;
	a->type = m;

	return a;
}

Arpent*
arpget(Block *bp, int version, Medium *type, uchar *ip, uchar *mac)
{
	int hash;
	Arpent *a;
	uchar v6ip[IPaddrlen];

	if(version == V4) {
		v4tov6(v6ip, ip);
		ip = v6ip;
	}

	qlock(&arp);
	hash = haship(ip);
	for(a = arp.hash[hash]; a; a = a->hash) {
		if(memcmp(ip, a->ip, sizeof(a->ip)) == 0)
		if(type == a->type)
			break;
	}

	if(a == nil){
		a = newarp(ip, type);
		a->state = AWAIT;
	}
	a->used = msec;
	if(a->state == AWAIT){
		if(a->hold)
			a->last->list = bp;
		else
			a->hold = bp;
		a->last = bp;
		bp->list = nil; 
		return a;		/* return with arp qlocked */
	}

	memmove(mac, a->mac, a->type->maclen);
	qunlock(&arp);
	return nil;
}

/*
 * called with arp locked
 */
void
arprelease(Arpent*)
{
	qunlock(&arp);
}

/*
 * called with arp locked
 */
Block*
arpresolve(Arpent *a, Medium *type, uchar *mac)
{
	Block *bp;

	memmove(a->mac, mac, type->maclen);
	a->type = type;
	a->state = AOK;
	a->used = msec;
	bp = a->hold;
	a->hold = nil;
	qunlock(&arp);

	return bp;
}

void
arpenter(Ipifc *ifc, int version, uchar *ip, uchar *mac, Medium *type, int refresh)
{
	Arpent *a;
	Block *bp, *next;
	uchar v6ip[IPaddrlen];

	if(version == V4){
		v4tov6(v6ip, ip);
		ip = v6ip;
	}

	qlock(&arp);
	for(a = arp.hash[haship(ip)]; a; a = a->hash) {
		if(a->type != type || (a->state != AWAIT && a->state != AOK))
			continue;

		if(ipcmp(a->ip, ip) == 0) {
			a->state = AOK;
			memmove(a->mac, mac, type->maclen);
			bp = a->hold;
			a->hold = nil;
			if(version == V4)
				ip += IPv4off;
			qunlock(&arp);
			while(bp) {
				next = bp->list;
				if(ifc != nil){
					if(waserror()){
						runlock(ifc);
						nexterror();
					}
					rlock(ifc);
					if(ifc->m != nil)
						ifc->m->bwrite(ifc, bp, version, ip);
					else
						freeb(bp);
					runlock(ifc);
					poperror();
				} else
					freeb(bp);
				bp = next;
			}
			a->used = msec;
			return;
		}
	}

	/* if nil, we're adding a new entry */
	if(refresh == 0){
		a = newarp(ip, type);
		a->state = AOK;
		a->type = type;
		memmove(a->mac, mac, type->maclen);
	}

	qunlock(&arp);
}

int
arpwrite(char *s, int len)
{
	int n;
	Block *bp;
	Arpent *a;
	char *f[4], buf[256];
	uchar ip[], mac[MAClen];
	Medium *m;

	if(len == 0)
		error(Ebadarp);
	if(len >= sizeof(buf))
		len = sizeof(buf)-1;
	strncpy(buf, s, len);
	buf[len] = 0;
	if(len > 0 && buf[len-1] == '\n')
		buf[len-1] = 0;

	n = parsefields(buf, f, 4, " ");
	if(strcmp(f[0], "flush") == 0){
		qlock(&arp);
		for(a = arp.cache; a < &arp.cache[NCACHE]; a++){
			memset(a->ip, 0, sizeof(a->ip));
			memset(a->mac, 0, sizeof(a->mac));
			a->hash = nil;
			a->state = 0;
			a->used = 0;
			while(a->hold != nil) {
				bp = a->hold->list;
				freeblist(a->hold);
				a->hold = bp;
			}
		}
		memset(arp.hash, 0, sizeof(arp.hash));
		qunlock(&arp);
	} else if(strcmp(f[0], "add") == 0){
		if(n != 4)
			error(Ebadarp);
		m = ipfindmedium(f[1]);
		if(m == nil)
			error(Ebadarp);
		parseip(ip, f[2]);
		parsemac(mac, f[3], m->maclen);
		arpenter(nil, V6, ip, mac, m, 0);
	} else
		error(Ebadarp);

	return len;
}

enum
{
	Alinelen=	66,
};

char *aformat = "%-6.6s %-8.8s %-16.16I %-32.32s\n";

static void
convmac(char *p, uchar *mac, int n)
{
	while(n-- > 0)
		p += sprint(p, "%2.2ux", *mac++);
}

int
arpread(char *p, ulong offset, int len)
{
	Arpent *a;
	int n;
	char mac[2*MAClen+1];

	if(offset % Alinelen)
		return 0;

	offset = offset/Alinelen;
	len = len/Alinelen;

	n = 0;
	for(a = arp.cache; len > 0 && a < &arp.cache[NCACHE]; a++){
		if(a->state == 0)
			continue;
		if(offset > 0){
			offset--;
			continue;
		}
		len--;
		qlock(&arp);
		convmac(mac, a->mac, a->type->maclen);
		n += sprint(p+n, aformat, a->type->name, arpstate[a->state], a->ip, mac);
		qunlock(&arp);
	}

	return n;
}
