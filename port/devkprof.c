#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"devtab.h"

#define	LRES	3		/* log of PC resolution */

struct
{
	int	minpc;
	int	maxpc;
	int	nbuf;
	int	time;
	ulong	*buf;
}kprof;

enum{
	Kprofdirqid,
	Kprofdataqid,
	Kprofctlqid,
	Nkproftab=Kprofctlqid,
	Kprofmaxqid,
};
Dirtab kproftab[Nkproftab]={
	"kpdata",	{Kprofdataqid},		0,	0600,
	"kpctl",	{Kprofctlqid},		0,	0600,
};

void kproftimer(ulong);

void
kprofreset(void)
{
	kprof.minpc = KTZERO;
	kprof.maxpc = (ulong)&etext;
	kprof.nbuf = (kprof.maxpc-kprof.minpc) >> LRES;
	kprof.buf = ialloc(conf.nmach * kprof.nbuf*sizeof kprof.buf[0], 0);
	kproftab[0].length = conf.nmach * kprof.nbuf*sizeof kprof.buf[0];
}

void
kprofinit(void)
{
}

Chan *
kprofattach(char *spec)
{
	return devattach('T', spec);
}
Chan *
kprofclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
kprofwalk(Chan *c, char *name)
{
	return devwalk(c, name, kproftab, (long)Nkproftab, devgen);
}

void
kprofstat(Chan *c, char *db)
{
	devstat(c, db, kproftab, (long)Nkproftab, devgen);
}

Chan *
kprofopen(Chan *c, int omode)
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
kprofcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
kprofremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
kprofwstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

void
kprofclose(Chan *c)
{
	USED(c);
}

long
kprofread(Chan *c, void *a, long n, ulong offset)
{
	ulong end;
	switch((int)(c->qid.path&~CHDIR)){
	case Kprofdirqid:
		return devdirread(c, a, n, kproftab, Nkproftab, devgen);
	case Kprofdataqid:
		end = conf.nmach * kprof.nbuf*sizeof kprof.buf[0];
		if(offset >= end){
			n = 0;
			break;
		}
		if(offset+n > end)
			n = end-offset;
		memmove(a, ((char *)kprof.buf)+offset, n);
		break;
	default:
		n=0;
		break;
	}
	return n;
}

long
kprofwrite(Chan *c, char *a, long n, ulong offset)
{
	switch((int)(c->qid.path&~CHDIR)){
	case Kprofctlqid:
		if(strncmp(a, "startclr", 8) == 0){
			memset((char *)kprof.buf, 0, conf.nmach * kprof.nbuf*sizeof kprof.buf[0]);
			kprof.time = 1;
		}else if(strncmp(a, "start", 5) == 0)
			kprof.time = 1;
		else if(strncmp(a, "stop", 4) == 0)
			kprof.time = 0;
		break;
	default:
		error(Ebadusefd);
	}
	return n;
}

void
kproftimer(ulong pc)
{
	extern void spldone(void);
	ulong *buf;

	if(kprof.time == 0)
		return;
	/*
	 *  if the pc is coming out of slplo pr splx, then use
	 *  the pc saved when we went splhi.
	 */
	if(pc>=(ulong)spllo && pc<=(ulong)spldone)
		pc = m->splpc;

	buf = kprof.buf+kprof.nbuf*m->machno;
	buf[0] += TK2MS(1);
	if(kprof.minpc<=pc && pc<kprof.maxpc){
		pc -= kprof.minpc;
		pc >>= LRES;
		buf[pc] += TK2MS(1);
	} else
		buf[1] += TK2MS(1);
}
