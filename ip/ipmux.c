#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

#define DPRINT if(0)print

typedef struct Iphdr		Iphdr;
typedef struct Ipmuxrock	Ipmuxrock;
typedef struct Ipmux		Ipmux;

enum
{
	IPHDR		= 20,		/* sizeof(Iphdr) */
};

struct Iphdr
{
	uchar	vihl;		/* Version and header length */
	uchar	tos;		/* Type of service */
	uchar	length[2];	/* packet length */
	uchar	id[2];		/* ip->identification */
	uchar	frag[2];	/* Fragment information */
	uchar	ttl;		/* Time to live */
	uchar	proto;		/* Protocol */
	uchar	cksum[2];	/* Header checksum */
	uchar	src[4];		/* IP source */
	uchar	dst[4];		/* IP destination */
	uchar	data[1];	/* start of data */
};

enum
{
	Tproto,
	Tdata,
	Tdst,
	Tsrc,
	Tifc,
};

char *ftname[] = 
{
[Tproto]	"proto",
[Tdata]		"data",
[Tdst]		"dst",
[Tsrc]		"src",
[Tifc]		"ifc",
};

/*
 *  a node in the decision tree
 */
struct Ipmux
{
	Ipmux	*yes;
	Ipmux	*no;
	uchar	type;
	ushort	len;		/* length in bytes of item to compare */
	ushort	off;		/* offset of comparison */
	int	n;		/* number of items val points to */
	uchar	*val;
	uchar	*mask;

	int	ref;		/* so we can garbage collect */
};

/*
 *  someplace to hold per conversation data
 */
struct Ipmuxrock
{
	Ipmux	*chain;
	int	proto;
};

static char*
skipwhite(char *p)
{
	while(*p == ' ' || *p == '\t')
		p++;
	return p;
}

static char*
follows(char *p, char c)
{
	char *f;

	f = strchr(p, c);
	if(f == nil)
		return nil;
	*f++ = 0;
	f = skipwhite(f);
	if(*f == 0)
		return nil;
	return f;
}

static Ipmux*
parseop(char **pp)
{
	char *p = *pp;
	int type, off, end, len;
	Ipmux *f;

	off = 0;
	p = skipwhite(p);
	if(strncmp(p, "dst", 3) == 0){
		type = Tdst;
		len = IPaddrlen;
		p += 3;
	}
	else if(strncmp(p, "src", 3) == 0){
		type = Tsrc;
		len = IPaddrlen;
		p += 3;
	}
	else if(strncmp(p, "ifc", 3) == 0){
		type = Tifc;
		len = IPaddrlen;
		p += 3;
	}
	else if(strncmp(p, "proto", 5) == 0){
		type = Tproto;
		len = 1;
		p += 5;
	}
	else if(strncmp(p, "data", 4) == 0){
		type = Tdata;
		p += 4;
		p = skipwhite(p);
		if(*p != '[')
			return nil;
		p++;
		off = strtoul(p, &p, 0);
		p = skipwhite(p);
		if(*p != ':')
			end = off;
		else {
			p++;
			p = skipwhite(p);
			end = strtoul(p, &p, 0);
			if(end < off)
				return nil;
			p = skipwhite(p);
		}
		if(*p != ']')
			return nil;
		len = end - off + 1;
		p++;
	}
	else 
		return nil;

	f = smalloc(sizeof(*f));
	f->type = type;
	f->len = len;
	f->off = off;
	f->val = nil;
	f->mask = nil;
	f->n = 1;
	f->ref = 1;

	return f;	
}

static int
htoi(char x)
{
	if(x >= '0' && x <= '9')
		x -= '0';
	else if(x >= 'a' && x <= 'f')
		x -= 'a' - 10;
	else if(x >= 'A' && x <= 'F')
		x -= 'A' - 10;
	else
		x = 0;
	return x;
}

static int
hextoi(char *p)
{
	return (htoi(p[0])<<4) | htoi(p[1]);
}

static void
parseval(uchar *v, char *p, int len)
{
	while(*p && len-- > 0){
		*v++ = hextoi(p);
		p += 2;
	}
}

static Ipmux*
parsedemux(char *p)
{
	int n;
	Ipmux *f;
	char *val;
	char *mask;
	char *vals[20];
	uchar *v;

	/* parse operand */
	f = parseop(&p);
	if(f == nil)
		return nil;

	/* find value */
	val = follows(p, '=');
	if(val == nil)
		goto parseerror;

	/* parse mask */
	mask = follows(p, '&');
	if(mask != nil){
		switch(f->type){
		case Tsrc:
		case Tdst:
		case Tifc:
			f->mask = smalloc(f->len);
			parseipmask(f->mask, mask);
			break;
		case Tdata:
			f->mask = smalloc(f->len);
			parseval(f->mask, mask, f->len);
			break;
		default:
			goto parseerror;
		}
	} else
		f->mask = nil;

	/* parse vals */
	f->n = tokenize(val, vals, sizeof(vals)/sizeof(char*));
	if(f->n == 0)
		goto parseerror;
	f->val = smalloc(f->n*f->len);
	v = f->val;
	for(n = 0; n < f->n; n++){
		switch(f->type){
		case Tsrc:
		case Tdst:
		case Tifc:
			parseip(v, vals[n]);
			break;
		case Tproto:
		case Tdata:
			parseval(v, vals[n], f->len);
			break;
		}
		v += f->len;
	}

	return f;

parseerror:
	if(f->mask)
		free(f->mask);
	if(f->val)
		free(f->val);
	free(f);
	return nil;
}

/*
 *  Compare relative ordering of two ipmuxs.  This doesn't compare the
 *  values, just the fields being looked at.  
 *
 *  returns:	<0 if a is a more specific match
 *		 0 if a and b are matching on the same fields
 *		>0 if b is a more specific match
 */
static int
ipmuxcmp(Ipmux *a, Ipmux *b)
{
	int n;

	/* compare types, lesser ones are more important */
	n = a->type - b->type;
	if(n != 0)
		return n;

	/* compare offsets, call earlier ones more specific */
	n = a->off - b->off;
	if(n != 0)
		return n;

	/* compare match lengths, longer ones are more specific */
	n = b->len - a->len;
	if(n != 0)
		return n;

	/*
	 *  if we get here we have two entries matching
	 *  the same bytes of the record.  Now check
	 *  the mask for equality.  Longer masks are
	 *  more specific.
	 */
	if(a->mask != nil && b->mask == nil)
		return -1;
	if(a->mask == nil && b->mask != nil)
		return 1;
	if(a->mask != nil && b->mask != nil){
		n = memcmp(b->mask, a->mask, a->len);
		if(n != 0)
			return n;
	}
	return 0;
}

/*
 *  Compare the values of two ipmuxs.  We're assuming that ipmuxcmp
 *  returned 0 comparing them.
 */
static int
ipmuxvalcmp(Ipmux *a, Ipmux *b)
{
	int n;

	n = b->len*b->n - a->len*a->n;
	if(n != 0)
		return n;
	return memcmp(a->val, b->val, a->len*a->n);
} 

/*
 *  add onto an existing ipmux chain in the canonical comparison
 *  order
 */
static void
ipmuxchain(Ipmux **l, Ipmux *f)
{
	for(; *l; l = &(*l)->yes)
		if(ipmuxcmp(f, *l) < 0)
			break;
	f->yes = *l;
	*l = f;
}

/*
 *  copy a tree
 */
static Ipmux*
ipmuxcopy(Ipmux *f)
{
	Ipmux *nf;

	if(f == nil)
		return nil;
	nf = smalloc(sizeof *nf);
	*nf = *f;
	nf->no = ipmuxcopy(f->no);
	nf->yes = ipmuxcopy(f->yes);
	nf->val = smalloc(f->n*f->len);
	memmove(nf->val, f->val, f->n*f->len);
	return nf;
}

static void
ipmuxfree(Ipmux *f)
{
	if(f->val != nil)
		free(f->val);
	free(f);
}

static void
ipmuxtreefree(Ipmux *f)
{
	if(f->no != nil)
		ipmuxfree(f->no);
	if(f->yes != nil)
		ipmuxfree(f->yes);
	ipmuxfree(f);
}

/*
 *  merge two trees
 */
static Ipmux*
ipmuxmerge(Ipmux *a, Ipmux *b)
{
	int n;
	Ipmux *f;

	if(a == nil)
		return b;
	if(b == nil)
		return a;
	n = ipmuxcmp(a, b);
	if(n < 0){
		f = ipmuxcopy(b);
		a->yes = ipmuxmerge(a->yes, b);
		a->no = ipmuxmerge(a->no, f);
		return a;
	}
	if(n > 0){
		f = ipmuxcopy(a);
		b->yes = ipmuxmerge(b->yes, a);
		b->no = ipmuxmerge(b->no, f);
		return b;
	}
	if(ipmuxvalcmp(a, b) == 0){
		a->yes = ipmuxmerge(a->yes, b->yes);
		a->no = ipmuxmerge(a->no, b->no);
		a->ref++;
		ipmuxfree(b);
		return a;
	}
	a->no = ipmuxmerge(a->no, b);
	return a;
}

/*
 *  remove a chain from a demux tree.  This is like merging accept that
 *  we remove instead of insert.
 */
static int
ipmuxremove(Ipmux **l, Ipmux *f)
{
	int n, rv;
	Ipmux *ft;

	if(f == nil)
		return 0;		/* we've removed it all */
	if(*l == nil)
		return -1;

	ft = *l;
	n = ipmuxcmp(ft, f);
	if(n < 0){
		/* *l is maching an earlier field, descend both paths */
		rv = ipmuxremove(&ft->yes, f);
		rv += ipmuxremove(&ft->no, f);
		return rv;
	}
	if(n > 0){
		/* f represents an earlier field than *l, this should be impossible */
		return -1;
	}

	/* if we get here f and *l are comparing the same fields */
	if(ipmuxvalcmp(ft, f) != 0){
		/* different values mean mutually exclusive */
		return ipmuxremove(&ft->no, f);
	}

	/* we found a match */
	if(--(ft->ref) == 0){
		/*
		 *  a dead node implies the whole yes side is also dead.
		 *  since our chain is constrained to be on that side,
		 *  we're done.
		 */
		ipmuxtreefree(ft->yes);
		*l = ft->no;
		ipmuxfree(ft);
		return 0;
	}

	/*
	 *  free the rest of the chain.  it is constrained to match the
	 *  yes side.
	 */
	return ipmuxremove(&ft->yes, f->yes);
}

/*
 *  connection request is a semi separated list of filters
 *  e.g. proto=17;dat[0:4]=11aa22bb;ifc=135.104.9.2
 *
 *  there's no protection against overlapping specs.
 */
static char*
ipmuxconnect(Conv *c, char **argv, int argc)
{
	int n, proto;
	char *field[10];
	Ipmux *mux, *chain;
	Ipmuxrock *r;
	Fs *f;

	f = c->p->f;

	if(argc != 2)
		return Ebadarg;

	n = parsefields(argv[1], field, nelem(field), ";");
	if(n <= 0)
		return Ebadarg;

	chain = nil; 
	for(i = 0; i < n; i++){
		mux = ipmuxparse(field[i]);
		if(mux == nil){
			ipmuxtreefree(chain);
			return Ebadarg;
		}
		ipmuxchain(&chain, mux);
	}

	/* optimize the protocol into an array lookup */
	if(chain->type != Tproto){
		ipmuxtreefree(chain);
		return "need proto rule";
	}
	mux = chain;
	proto = mux->val;
	chain = chain->yes;
	ipmuxfree(mux);

	/* save a copy of the chain so we can later remove it */
	mux->conv = c;
	mux = ipmuxcopy(chain);
	r = (Ipmuxrock*)(c->ptcl);
	r->chain = chain;
	r->proto = proto;

	/* add the chain to the protocol demultiplexor tree */
	wlock(f);
	f->t2m[proto] = ipmuxmerge(f->t2m[proto], mux);
	wunlock(f);

	Fsconnected(c, nil);
	return nil;
}

static int
ipmuxstate(Conv *c, char *state, int n)
{
	USED(c);
	return snprint(state, n, "%s", "Datagram");
}

static void
ipmuxcreate(Conv *c)
{
	c->rq = qopen(64*1024, 0, 0, c);
	c->wq = qopen(64*1024, 0, 0, 0);
	*(IPmux**)(c->ptcl) = nil;
}

static char*
ipmuxannounce(Conv*, char**, int)
{
	return "ipmux does not support announce";
}

static void
ipmuxclose(Conv *c)
{
	Ipmuxrock *r;

	r = (Ipmuxrock*)(c->ptcl);
	r->chain = chain;
	r->proto = proto;

	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
	c->lport = 0;
	c->rport = 0;

	wlock(f);
	ipmuxremove(&(f->t2m[r->proto]), r->chain);
	wunlock(f);
	ipmuxtreefree(f->chain);

	unlock(c);
}

/*
 *  takes a fully formed ip packet and just passes it down
 *  the stack
 */
static void
ipmuxkick(Conv *c, int l)
{
}

static void
ipmuxiput(Proto *p, uchar *ia, Block *bp)
{
	int len;
	Iphdr *ip;
	Fs *f = p->f;
	uchar *p;
	Conv *c;

	ip = bp->rptr;
	rlock(f);
	mux = f->t2m[ip->proto];
	if(mux == nil)
		goto out;

	/* run the v4 filter */
	len = BLEN(bp);
	if(len < 64 && bp->next){
		bp = concatblock(bp);
		len = BLEN(bp);
	}
	c = nil;
	while(mux != nil){
		while(mux){
			switch(mux->type){
			case Tia:
				p = ia;
				break;
			case Tsrc:
				p = ip->src;
				break;
			case Tdst:
				p = ip->dst;
				break;
			case Tdata:
				p = ip->data;
				if(mux->off+mux->len > len)
					goto no;
				break;
			}
		}
		if(mux->mask != nil){
		} else {
		}
no:
		mux = mux->no;
		continue;
	}
out:
	/* doesn't match any filter, hand it to the specific protocol handler */
	runlock(f);
	p = f->t2p[ip->proto];
	if(p)
		(*p->rcv)(p, ia, bp);
	else
		freeblist(bp);
	return;	
}

int
ipmuxstats(Proto *p, char *buf, int len)
{
	return 0;
}

void
ipmuxinit(Fs *fs)
{
	Proto *ipmux;

	ipmux = smalloc(sizeof(Proto));
	ipmux->priv = nil;
	ipmux->name = "ipmux";
	ipmux->kick = ipmuxkick;
	ipmux->connect = ipmuxconnect;
	ipmux->announce = ipmuxannounce;
	ipmux->state = ipmuxstate;
	ipmux->create = ipmuxcreate;
	ipmux->close = ipmuxclose;
	ipmux->rcv = ipmuxiput;
	ipmux->ctl = nil;
	ipmux->advise = nil;
	ipmux->stats = ipmuxstats;
	ipmux->ipproto = -1;
	ipmux->nc = 64;
	ipmux->ptclsize = sizeof(Ipmuxrock);

	Fsproto(fs, ipmux);
}
