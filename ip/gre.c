#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"

#define DPRINT if(0)print

enum
{
	GRE_IPHDRSIZE	= 20,		/* size of ip header */
	GRE_HDRSIZE	= 4,		/* minimum size of GRE header */
	IP_GREPROTO	= 47,

	GRErxms		= 200,
	GREtickms	= 100,
	GREmaxxmit	= 10,
};

typedef struct GREhdr
{
	/* ip header */
	byte	vihl;		/* Version and header length */
	byte	tos;		/* Type of service */
	byte	len[2];		/* packet length (including headers) */
	byte	id[2];		/* Identification */
	byte	frag[2];	/* Fragment information */
	byte	Unused;	
	byte	proto;		/* Protocol */
	byte	cksum[2];	/* checksum */
	byte	src[4];		/* Ip source */
	byte	dst[4];		/* Ip destination */

	/* gre header */
	byte	flags[2];
	byte	eproto[2];	/* encapsulation protocol */
} GREhdr;

	Proto	gre;
extern	Fs	fs;
	int	gredebug;

static char*
greconnect(Conv *c, char **argv, int argc)
{
	Proto *p;
	char *err;
	Conv *tc, **cp, **ecp;

	err = Fsstdconnect(c, argv, argc);
	if(err != nil)
		return err;

	/* make sure noone's already connected to this other sys */
	p = c->p;
	lock(p);
	ecp = &p->conv[p->nc];
	for(cp = p->conv; cp < ecp; cp++){
		tc = *cp;
		if(tc == nil)
			break;
		if(tc == c)
			continue;
		if(tc->rport == c->rport && tc->raddr == c->raddr){
			err = "already connected to that addr/proto";
			c->rport = 0;
			c->raddr = 0;
			break;
		}
	}
	unlock(p);

	if(err != nil)
		return err;
	Fsconnected(&fs, c, nil);

	return nil;
}

static int
grestate(char **msg, Conv *c)
{
	USED(c);
	*msg = "Datagram";
	return 1;
}

static void
grecreate(Conv *c)
{
	c->rq = qopen(64*1024, 0, 0, c);
	c->wq = qopen(64*1024, 0, 0, 0);
}

static char*
greannounce(Conv*, char**, int)
{
	return "pktifc does not support announce";
}

static void
greclose(Conv *c)
{
	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	c->laddr = 0;
	c->raddr = 0;
	c->lport = 0;
	c->rport = 0;

	unlock(c);
}

int drop;

static void
grekick(Conv *c, int l)
{
	GREhdr *ghp;
	Block *bp, *f;
	int dlen;

	USED(l);

	bp = qget(c->wq);
	if(bp == nil)
		return;

	/* Round packet up to even number of bytes */
	dlen = blocklen(bp);
	if(dlen & 1) {
		for(f = bp; f->next; f = f->next)
			;
		if(f->wp >= f->lim) {
			f->next = allocb(1);
			f = f->next;
		}
		*f->wp++ = 0;
	}

	/* Make space to fit ip header (gre header already there) */
	bp = padblock(bp, GRE_IPHDRSIZE);
	if(bp == nil)
		return;

	/* make sure the message has a GRE header */
	bp = pullupblock(bp, GRE_IPHDRSIZE+GRE_HDRSIZE);
	if(bp == nil)
		return;

	ghp = (GREhdr *)(bp->rp);

	ghp->proto = IP_GREPROTO;
	hnputl(ghp->dst, c->raddr);
	hnputl(ghp->src, c->laddr);
	hnputs(ghp->eproto, c->rport);

	ipoput(bp, 0, c->ttl);
}

static void
greiput(Media *m, Block *bp)
{
	int len;
	GREhdr *ghp;
	Ipaddr addr;
	Conv *c, **p;
	ushort eproto;

	USED(m);
	ghp = (GREhdr*)(bp->rp);

	eproto = nhgets(ghp->eproto);
	addr = nhgetl(ghp->src);

	/* Look for a conversation structure for this port and address */
	c = nil;
	for(p = gre.conv; *p; p++) {
		c = *p;
		if(c->inuse == 0)
			continue;
		if(c->raddr == addr && c->rport == eproto)
			break;
	}

	if(*p == nil) {
		freeblist(bp);
		return;
	}

	/*
	 * Trim the packet down to data size
	 */
	len = nhgets(ghp->len) - GRE_IPHDRSIZE;
	if(len < GRE_HDRSIZE){
		freeblist(bp);
		return;
	}
	bp = trimblock(bp, GRE_IPHDRSIZE, len);
	if(bp == nil){
		gre.lenerr++;
		return;
	}

	/*
	 *  Can't delimit packet so pull it all into one block.
	 */
	if(qlen(c->rq) > 64*1024)
		freeblist(bp);
	else{
		bp = concatblock(bp);
		if(bp == 0)
			panic("greiput");
		qpass(c->rq, bp);
	}
}

void
greinit(Fs *fs)
{
	gre.name = "gre";
	gre.kick = grekick;
	gre.connect = greconnect;
	gre.announce = greannounce;
	gre.state = grestate;
	gre.create = grecreate;
	gre.close = greclose;
	gre.rcv = greiput;
	gre.ctl = nil;
	gre.advise = nil;
	gre.ipproto = IP_GREPROTO;
	gre.nc = 64;
	gre.ptclsize = 0;

	Fsproto(fs, &gre);
}
