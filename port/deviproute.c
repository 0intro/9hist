#include <u.h>
#include <libc.h>

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
/*	Lock; */
	Iproute *first;
	Iproute	r[Nroutes];	/* routings */
};
Iprtab	iprtab;

#define lock(x)
#define unlock(x)

void
printroute(void)
{
	Iproute *r;

	print("\n");
	for(r = iprtab.first; r; r = r->next)
		print("%d.%d.%d.%d  %d.%d.%d.%d  %d.%d.%d.%d\n",
			r->dst[0], r->dst[1], r->dst[2], r->dst[3],
			r->mask[0], r->mask[1], r->mask[2], r->mask[3],
			r->gate[0], r->gate[1], r->gate[2], r->gate[3]);
	print("\n");
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
			return;

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
	unlock(&iprtab);
}
