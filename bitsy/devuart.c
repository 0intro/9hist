#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"../port/netif.h"


static	Uart*	uart[Nuart];
static	int	nuart;

static Dirtab *uartdir;
static int uartndir;

/*
 * means the kernel is using this as a console
 */
static char	Ekinuse[] = "device in use by kernel";

static void	uartdcdhup(Uart*, int);
static void	uartdcdts(Uart*, int);
static void	uartdsrhup(Uart*, int);

/*
 *  define a Uart
 */
Uart*
uartsetup(PhysUart *phys, void *regs, ulong freq, char *name)
{
	Uart *p;

	if(nuart >= Nuart)
		return nil;

	p = xalloc(sizeof(Uart));
	uart[nuart] = p;
	strcpy(p->name, name);
	p->dev = nuart++;
	p->freq = freq;
	p->regs = regs;
	p->phys = phys;

	/*
	 *  set rate to 115200 baud.
	 *  8 bits/character.
	 *  1 stop bit.
	 *  enabled with interrupts disabled.
	 */
	(*p->phys->bits)(p, 8);
	(*p->phys->stop)(p, 1);
	(*p->phys->baud)(p, 115200);
	(*p->phys->enable)(p, 0);

	p->iq = qopen(4*1024, 0, p->phys->flow, p);
	p->oq = qopen(4*1024, 0, p->phys->kick, p);
	if(p->iq == nil || p->oq == nil)
		panic("uartsetup");

	p->ip = p->istage;
	p->ie = &p->istage[Stagesize];
	p->op = p->ostage;
	p->oe = p->ostage;
	return p;
}

static void
setlength(int i)
{
	Uart *p;

	if(i > 0){
		p = uart[i];
		if(p && p->opens && p->iq)
			uartdir[3*i].length = qlen(p->iq);
	} else for(i = 0; i < nuart; i++){
		p = uart[i];
		if(p && p->opens && p->iq)
			uartdir[3*i].length = qlen(p->iq);
	}
}

/*
 *  setup the '#t' directory
 */
static void
uartreset(void)
{
	int i;
	Dirtab *dp;

	uartndir = 3*nuart;
	uartdir = xalloc(uartndir * sizeof(Dirtab));
	dp = uartdir;
	for(i = 0; i < nuart; i++){
		/* 3 directory entries per port */
		sprint(dp->name, "eia%d", i);
		dp->qid.path = NETQID(i, Ndataqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dctl", i);
		dp->qid.path = NETQID(i, Nctlqid);
		dp->perm = 0660;
		dp++;
		sprint(dp->name, "eia%dstat", i);
		dp->qid.path = NETQID(i, Nstatqid);
		dp->perm = 0444;
		dp++;
	}
}


static Chan*
uartattach(char *spec)
{
	return devattach('t', spec);
}

static int
uartwalk(Chan *c, char *name)
{
	return devwalk(c, name, uartdir, uartndir, devgen);
}

static void
uartstat(Chan *c, char *dp)
{
	if(NETTYPE(c->qid.path) == Ndataqid)
		setlength(NETID(c->qid.path));
	devstat(c, dp, uartdir, uartndir, devgen);
}

static Chan*
uartopen(Chan *c, int omode)
{
	Uart *p;

	c = devopen(c, omode, uartdir, uartndir, devgen);

	switch(NETTYPE(c->qid.path)){
	case Nctlqid:
	case Ndataqid:
		p = uart[NETID(c->qid.path)];
		if(p->kinuse)
			error(Ekinuse);
		qlock(p);
		if(p->opens++ == 0){
			(*p->phys->enable)(p, 1);
			qreopen(p->iq);
			qreopen(p->oq);
		}
		qunlock(p);
		break;
	}

	return c;
}

static void
uartclose(Chan *c)
{
	Uart *p;

	if(c->qid.path & CHDIR)
		return;
	if((c->flag & COPEN) == 0)
		return;
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
	case Nctlqid:
		p = uart[NETID(c->qid.path)];
		if(p->kinuse)
			error(Ekinuse);
		qlock(p);
		if(--(p->opens) == 0){
			(*p->phys->disable)(p);
			qclose(p->iq);
			qclose(p->oq);
			p->ip = p->istage;
			p->dcd = p->dsr = p->dohup = 0;
		}
		qunlock(p);
		break;
	}
}

static long
uartread(Chan *c, void *buf, long n, vlong off)
{
	Uart *p;
	ulong offset = off;

	if(c->qid.path & CHDIR){
		setlength(-1);
		return devdirread(c, buf, n, uartdir, uartndir, devgen);
	}

	p = uart[NETID(c->qid.path)];
	if(p->kinuse)
		error(Ekinuse);
	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qread(p->iq, buf, n);
	case Nctlqid:
		return readnum(offset, buf, n, NETID(c->qid.path), NUMSIZE);
	case Nstatqid:
		return (*p->phys->status)(p, buf, n, offset);
	}

	return 0;
}

static void
uartctl(Uart *p, char *cmd)
{
	int i, n;
	char *f[32];
	int nf;

	/* let output drain for a while */
	for(i = 0; i < 16 && qlen(p->oq); i++)
		tsleep(&p->r, (int(*)(void*))qlen, p->oq, 125);

	nf = getfields(cmd, f, nelem(f), 1, " \t\n");

	for(i = 0; i < nf; i++){

		if(strncmp(f[i], "break", 5) == 0){
			(*p->phys->dobreak)(p, 0);
			continue;
		}

		n = atoi(f[i]+1);
		switch(*f[i]){
		case 'B':
		case 'b':
			(*p->phys->baud)(p, n);
			break;
		case 'C':
		case 'c':
			uartdcdhup(p, n);
			break;
		case 'D':
		case 'd':
			(*p->phys->dtr)(p, n);
			break;
		case 'E':
		case 'e':
			uartdsrhup(p, n);
			break;
		case 'f':
		case 'F':
			qflush(p->oq);
			break;
		case 'H':
		case 'h':
			qhangup(p->iq, 0);
			qhangup(p->oq, 0);
			break;
		case 'i':
		case 'I':
			break;
		case 'L':
		case 'l':
			(*p->phys->bits)(p, n);
			break;
		case 'm':
		case 'M':
			(*p->phys->modemctl)(p, n);
			break;
		case 'n':
		case 'N':
			qnoblock(p->oq, n);
			break;
		case 'P':
		case 'p':
			(*p->phys->parity)(p, *(cmd+1));
			break;
		case 'K':
		case 'k':
			(*p->phys->dobreak)(p, n);
			break;
		case 'R':
		case 'r':
			(*p->phys->rts)(p, n);
			break;
		case 'Q':
		case 'q':
			qsetlimit(p->iq, n);
			qsetlimit(p->oq, n);
			break;
		case 'T':
		case 't':
			uartdcdts(p, n);
			break;
		case 'W':
		case 'w':
			/* obsolete */
			break;
		case 'X':
		case 'x':
			ilock(&p->tlock);
			p->xonoff = n;
			iunlock(&p->tlock);
			break;
		}
	}
}

static long
uartwrite(Chan *c, void *buf, long n, vlong)
{
	Uart *p;
	char cmd[32];

	if(c->qid.path & CHDIR)
		error(Eperm);

	p = uart[NETID(c->qid.path)];
	if(p->kinuse)
		error(Ekinuse);

	switch(NETTYPE(c->qid.path)){
	case Ndataqid:
		return qwrite(p->oq, buf, n);
	case Nctlqid:
		if(n >= sizeof(cmd))
			n = sizeof(cmd)-1;
		memmove(cmd, buf, n);
		cmd[n] = 0;
		uartctl(p, cmd);
		return n;
	}
}

static void
uartwstat(Chan *c, char *dp)
{
	Dir d;
	Dirtab *dt;

	if(!iseve())
		error(Eperm);
	if(CHDIR & c->qid.path)
		error(Eperm);
	if(NETTYPE(c->qid.path) == Nstatqid)
		error(Eperm);

	dt = &uartdir[3 * NETID(c->qid.path)];
	convM2D(dp, &d);
	d.mode &= 0666;
	dt[0].perm = dt[1].perm = d.mode;
}

Dev uartdevtab = {
	't',
	"uart",

	uartreset,
	devinit,
	uartattach,
	devclone,
	uartwalk,
	uartstat,
	uartopen,
	devcreate,
	uartclose,
	uartread,
	devbread,
	uartwrite,
	devbwrite,
	devremove,
	uartwstat,
};

/*
 * decide if we should hangup when dsr or dcd drops.
 */
static void
uartdsrhup(Uart *p, int n)
{
	p->hup_dsr = n;
}

static void
uartdcdhup(Uart *p, int n)
{
	p->hup_dcd = n;
}

/*
 *  save dcd timestamps for gps clock
 */
static void
uartdcdts(Uart *p, int n)
{
	p->dcdts = n;
}

/*
 *  put some bytes into the local queue to avoid calling
 *  qconsume for every character
 */
int
uartstageoutput(Uart *p)
{
	int n;

	n = qconsume(p->oq, p->ostage, Stagesize);
	if(n <= 0)
		return 0;
	p->op = p->ostage;
	p->oe = p->ostage + n;
	return n;
}
