#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"

#include	"devtab.h"

#define	MAXPC	(100*1024L)
#define	RES	8
#define	LRES	3
#define	NBUF	(MAXPC>>LRES)

long		timerbuf[NBUF];

enum{
	Kprofdirqid,
	Kprofdataqid,
	Kprofstartqid,
	Kprofstartclrqid,
	Kprofstopqid,
	Nkproftab=Kprofstopqid,
	Kprofmaxqid,
};
Dirtab kproftab[Nkproftab]={
	"kpdata",	Kprofdataqid,		NBUF*sizeof timerbuf[0],	0600,
	"kpstart",	Kprofstartqid,		0,		0600,
	"kpstartclr",	Kprofstartclrqid,	0,		0600,
	"kpstop",	Kprofstopqid,		0,		0600,
};

void
kprofreset(void)
{
}

void
kprofinit(void)
{
	extern void *etext;
	if((((unsigned long)&etext)-KTZERO)>MAXPC)
		print("kernel profiling limited to %lud\n", MAXPC);
}

Chan *
kprofattach(char *spec)
{
	return devattach('t', spec);
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
	if(c->qid == CHDIR){
		if(omode != OREAD)
			error(0, Eperm);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
kprofcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void
kprofremove(Chan *c)
{
	error(0, Eperm);
}

void
kprofwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
kprofclose(Chan *c)
{
}

void
kprofuserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
kproferrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

long
kprofread(Chan *c, void *a, long n)
{
	switch((int)(c->qid&~CHDIR)){
	case Kprofdirqid:
		return devdirread(c, a, n, kproftab, Nkproftab, devgen);
	case Kprofdataqid:
		if(c->offset >= NBUF*sizeof timerbuf[0]){
			n = 0;
			break;
		}
		if(c->offset+n > NBUF*sizeof timerbuf[0])
			n = NBUF*sizeof timerbuf[0]-c->offset;
		memcpy(a, ((char *)timerbuf)+c->offset, n);
		break;
	default:
		n=0;
		break;
	}
	return n;
}

long
kprofwrite(Chan *c, char *a, long n)
{
	switch((int)(c->qid&~CHDIR)){
	case Kprofstartclrqid:
		memset((char *)timerbuf, 0, NBUF*sizeof timerbuf[0]);
	case Kprofstartqid:
		duartstarttimer();
		break;
	case Kprofstopqid:
		duartstoptimer();
		break;
	default:
		error(0, Ebadusefd);
	}
	return n;
}

void
kproftimer(ulong pc)
{
	timerbuf[0]++;
	if(KTZERO<=pc && pc<KTZERO+MAXPC){
		pc -= KTZERO;
		pc >>= LRES;
		timerbuf[pc]++;
	}
}
