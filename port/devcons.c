#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"

Queue	*mouseq;
Queue	*kbdq;		/* unprocessed console input */
Queue	*lineq;		/* processed console input */
Queue	*printq;	/* console output */
Queue	*klogq;

static struct
{
	QLock;

	int	raw;		/* true if we shouldn't process input */
	int	ctl;		/* number of opens to the control file */
	int	x;		/* index into line */
	char	line[1024];	/* current input line */

	char	c;
	int	count;
	int	repeat;
} kbd;


char	sysname[NAMELEN];

void
printinit(void)
{
	klogq = qopen(32*1024, 0, 0, 0);
	lineq = qopen(2*1024, 0, 0, 0);
}

/*
 *   Print a string on the console.  Convert \n to \r\n for serial
 *   line consoles.  Locking of the queues is left up to the screen
 *   or uart code.  Multi-line messages to serial consoles may get
 *   interspersed with other messages.
 */
void
putstrn(char *str, int n)
{
	int m;
	char *t;
	char buf[PRINTSIZE+2];

	/*
	 *  if there's an attached bit mapped display,
	 *  put the message there.  screenputs is defined
	 *  as a null macro for systems that have no such
	 *  display.
	 */
	screenputs(str, n);

	/*
	 *  if there's a serial line being used as a console,
	 *  put the message there.
	 */
	if(printq == 0)
		return;

	while(n > 0) {
		t = memchr(str, '\n', n);
		if(t) {
			m = t - str;
			memmove(buf, str, m);
			buf[m] = '\r';
			buf[m+1] = '\n';
			qwrite(printq, buf, m+2, 1);
			str = t + 1;
			n -= m + 1;
		}
		else {
			qwrite(printq, str, n, 1);
			break;
		}
	}
}

int
sprint(char *s, char *fmt, ...)
{
	return doprint(s, s+PRINTSIZE, fmt, (&fmt+1)) - s;
}

int
print(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	putstrn(buf, n);

	return n;
}

int
kprint(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	qwrite(klogq, buf, n, 1);
	return n;
}

void
panic(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	strcpy(buf, "panic: ");
	n = doprint(buf+strlen(buf), buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	buf[n] = '\n';
	putstrn(buf, n+1);
	prflush();
	dumpstack();
	exit(1);
}
int
pprint(char *fmt, ...)
{
	char buf[2*PRINTSIZE];
	Chan *c;
	int n;

	if(up->fgrp == 0)
		return 0;

	c = up->fgrp->fd[2];
	if(c==0 || (c->mode!=OWRITE && c->mode!=ORDWR))
		return 0;
	n = sprint(buf, "%s %d: ", up->text, up->pid);
	n = doprint(buf+n, buf+sizeof(buf), fmt, (&fmt+1)) - buf;

	if(waserror())
		return 0;
	(*devtab[c->type].write)(c, buf, n, c->offset);
	poperror();

	lock(c);
	c->offset += n;
	unlock(c);

	return n;
}

void
prflush(void)
{
	while(qlen(printq) > 0) ;
}

void
echo(Rune r, char *buf, int n)
{
	static int ctrlt;

	/*
	 * ^p hack
	 */
	if(r==0x10 && cpuserver)
		exit(0);

	/*
	 * ^t hack BUG
	 */
	if(ctrlt == 2){
		ctrlt = 0;
		switch(r){
		case 0x14:
			break;	/* pass it on */
		case 'x':
			xsummary();
			ixsummary();
			break;
		case 'b':
			bitdebug();
			break;
		case 'd':
			consdebug();
			return;
		case 'p':
			procdump();
			return;
		case 'r':
			exit(0);
			break;
		}
	}else if(r == 0x14){
		ctrlt++;
		return;
	}
	ctrlt = 0;
	if(kbd.raw)
		return;

	/*
	 *  finally, the actual echoing
	 */
	if(r == 0x15)
		putstrn("^U\n", 3);
	else
		putstrn(buf, n);
}

/*
 *  Called by a uart interrupt for console input.
 *
 *  turn '\r' into '\n' before putting it into the queue.
 */
int
kbdcr2nl(Queue *q, int ch)
{
	if(ch == '\r')
		ch = '\n';
	return kbdputc(q, ch);
}

/*
 *  Put character, possibly a rune, into read queue at interrupt time.
 *  Called at interrupt time to process a character.
 */
int
kbdputc(Queue *q, int ch)
{
	int n;
	char buf[3];
	Rune r;

	USED(q);
	r = ch;
	n = runetochar(buf, &r);
	if(n == 0)
		return 0;
	echo(r, buf, n);
	qproduce(kbdq, buf, n);
	return 0;
}

void
kbdrepeat(int rep)
{
	kbd.repeat = rep;
	kbd.count = 0;
}

void
kbdclock(void)
{
	if(kbd.repeat == 0)
		return;
	if(kbd.repeat==1 && ++kbd.count>HZ){
		kbd.repeat = 2;
		kbd.count = 0;
		return;
	}
	if(++kbd.count&1)
		kbdputc(kbdq, kbd.c);
}

int
consactive(void)
{
	return qlen(printq) > 0;
}

enum{
	Qdir,
	Qauth,
	Qauthcheck,
	Qauthent,
	Qclock,
	Qcons,
	Qconsctl,
	Qcputime,
	Qhz,
	Qkey,
	Qhostdomain,
	Qhostowner,
	Qklog,
	Qlights,
	Qmsec,
	Qnoise,
	Qnoteid,
	Qnull,
	Qpgrpid,
	Qpid,
	Qppid,
	Qswap,
	Qsysname,
	Qsysstat,
	Qtime,
	Quser,
};

Dirtab consdir[]={
	"authenticate",	{Qauth},	0,		0666,
	"authcheck",	{Qauthcheck},	0,		0666,
	"authenticator", {Qauthent},	0,		0666,
	"clock",	{Qclock},	2*NUMSIZE,	0444,
	"cons",		{Qcons},	0,		0660,
	"consctl",	{Qconsctl},	0,		0220,
	"cputime",	{Qcputime},	6*NUMSIZE,	0444,
	"hostdomain",	{Qhostdomain},	DOMLEN,		0664,
	"hostowner",	{Qhostowner},	NAMELEN,	0664,
	"hz",		{Qhz},		NUMSIZE,	0666,
	"key",		{Qkey},		DESKEYLEN,	0622,
	"klog",		{Qklog},	0,		0444,
	"lights",	{Qlights},	0,		0220,
	"msec",		{Qmsec},	NUMSIZE,	0444,
	"noise",	{Qnoise},	0,		0220,
	"noteid",	{Qnoteid},	NUMSIZE,	0444,
	"null",		{Qnull},	0,		0666,
	"pgrpid",	{Qpgrpid},	NUMSIZE,	0444,
	"pid",		{Qpid},		NUMSIZE,	0444,
	"ppid",		{Qppid},	NUMSIZE,	0444,
	"swap",		{Qswap},	0,		0664,
	"sysname",	{Qsysname},	0,		0664,
	"sysstat",	{Qsysstat},	0,		0666,
	"time",		{Qtime},	NUMSIZE,	0664,
 	"user",		{Quser},	NAMELEN,	0666,
};

#define	NCONS	(sizeof consdir/sizeof(Dirtab))

ulong	boottime;		/* seconds since epoch at boot */

long
seconds(void)
{
	return boottime + TK2SEC(MACHP(0)->ticks);
}

int
readnum(ulong off, char *buf, ulong n, ulong val, int size)
{
	char tmp[64];
	Fconv fconv = (Fconv){ tmp, tmp+sizeof(tmp), size-1, 0, 0, 'u' };

	numbconv(&val, &fconv);
	tmp[size-1] = ' ';
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, tmp+off, n);
	return n;
}

int
readstr(ulong off, char *buf, ulong n, char *str)
{
	int size;

	size = strlen(str);
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, str+off, n);
	return n;
}

void
consreset(void)
{
}

void
consinit(void)
{
}

Chan*
consattach(char *spec)
{
	return devattach('c', spec);
}

Chan*
consclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
conswalk(Chan *c, char *name)
{
	return devwalk(c, name, consdir, NCONS, devgen);
}

void
consstat(Chan *c, char *dp)
{
	devstat(c, dp, consdir, NCONS, devgen);
}

Chan*
consopen(Chan *c, int omode)
{
	c->aux = 0;
	switch(c->qid.path){
	case Qconsctl:
		if(!iseve())
			error(Eperm);
		qlock(&kbd);
		kbd.ctl++;
		qunlock(&kbd);
		break;
	}
	return devopen(c, omode, consdir, NCONS, devgen);
}

void
conscreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
consclose(Chan *c)
{
	/* last close of control file turns off raw */
	switch(c->qid.path){
	case Qconsctl:
		if(c->flag&COPEN){
			qlock(&kbd);
			if(--kbd.ctl == 0)
				kbd.raw = 0;
			qunlock(&kbd);
		}
	case Qauth:
	case Qauthcheck:
	case Qauthent:
		authclose(c);
	}
}

long
consread(Chan *c, void *buf, long n, ulong offset)
{
	ulong l;
	Mach *mp;
	char *b, *bp;
	char tmp[128];		/* must be >= 6*NUMSIZE */
	char *cbuf = buf;
	int ch, i, k, id, eol;

	if(n <= 0)
		return n;
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, consdir, NCONS, devgen);

	case Qcons:
		qlock(&kbd);
		if(waserror()){
			qunlock(&kbd);
			nexterror();
		}
		while(!qcanread(lineq)){
			qread(kbdq, &kbd.line[kbd.x], 1);
			ch = kbd.line[kbd.x];
			if(kbd.raw){
				i = splhi();
				qproduce(lineq, &kbd.line[kbd.x], 1);
				splx(i);
				continue;
			}
			eol = 0;
			switch(ch){
			case '\b':
				if(kbd.x)
					kbd.x--;
				break;
			case 0x15:
				kbd.x = 0;
				break;
			case '\n':
			case 0x04:
				eol = 1;
			default:
				kbd.line[kbd.x++] = ch;
				break;
			}
			if(kbd.x == sizeof(kbd.line) || eol){
				if(ch == 0x04)
					kbd.x--;
				qwrite(lineq, kbd.line, kbd.x, 1);
				kbd.x = 0;
			}
		}
		n = qread(lineq, buf, n);
		qunlock(&kbd);
		poperror();
		return n;

	case Qcputime:
		k = offset;
		if(k >= 6*NUMSIZE)
			return 0;
		if(k+n > 6*NUMSIZE)
			n = 6*NUMSIZE - k;
		/* easiest to format in a separate buffer and copy out */
		for(i=0; i<6 && NUMSIZE*i<k+n; i++){
			l = up->time[i];
			if(i == TReal)
				l = MACHP(0)->ticks - l;
			l = TK2MS(l);
			readnum(0, tmp+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		memmove(buf, tmp+k, n);
		return n;

	case Qpgrpid:
		return readnum(offset, buf, n, up->pgrp->pgrpid, NUMSIZE);

	case Qnoteid:
		return readnum(offset, buf, n, up->noteid, NUMSIZE);

	case Qpid:
		return readnum(offset, buf, n, up->pid, NUMSIZE);

	case Qppid:
		return readnum(offset, buf, n, up->parentpid, NUMSIZE);

	case Qtime:
		return readnum(offset, buf, n, boottime+TK2SEC(MACHP(0)->ticks), 12);

	case Qclock:
		k = offset;
		if(k >= 2*NUMSIZE)
			return 0;
		if(k+n > 2*NUMSIZE)
			n = 2*NUMSIZE - k;
		readnum(0, tmp, NUMSIZE, MACHP(0)->ticks, NUMSIZE);
		readnum(0, tmp+NUMSIZE, NUMSIZE, HZ, NUMSIZE);
		memmove(buf, tmp+k, n);
		return n;

	case Qkey:
		return keyread(buf, n, offset);

	case Qauth:
		return authread(c, cbuf, n);

	case Qauthent:
		return authentread(c, cbuf, n);

	case Qhostowner:
		return readstr(offset, buf, n, eve);

	case Qhostdomain:
		return readstr(offset, buf, n, hostdomain);

	case Quser:
		return readstr(offset, buf, n, up->user);

	case Qnull:
		return 0;

	case Qklog:
		return qread(klogq, buf, n);

	case Qmsec:
		return readnum(offset, buf, n, TK2MS(MACHP(0)->ticks), NUMSIZE);

	case Qhz:
		return readnum(offset, buf, n, HZ, NUMSIZE);

	case Qsysstat:
		b = smalloc(conf.nmach*(NUMSIZE*8+1) + 1);	/* +1 for NUL */
		bp = b;
		for(id = 0; id < 32; id++) {
			if(active.machs & (1<<id)) {
				mp = MACHP(id);
				readnum(0, bp, NUMSIZE, id, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->cs, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->intr, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->syscall, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->pfault, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->tlbfault, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->tlbpurge, NUMSIZE);
				bp += NUMSIZE;
				readnum(0, bp, NUMSIZE, mp->load, NUMSIZE);
				bp += NUMSIZE;
				*bp++ = '\n';
			}
		}
		n = readstr(offset, buf, n, b);
		free(b);
		return n;

	case Qswap:
		sprint(tmp, "%d/%d memory %d/%d swap\n",
				palloc.user-palloc.freecount, palloc.user, 
				conf.nswap-swapalloc.free, conf.nswap);

		return readstr(offset, buf, n, tmp);

	case Qsysname:
		return readstr(offset, buf, n, sysname);

	default:
		print("consread %lux\n", c->qid);
		error(Egreg);
	}
	return -1;		/* never reached */
}

void
conslights(char *a, int n)
{
	char line[128];
	char *lp;
	int c;

	lp = line;
	while(n--){
		*lp++ = c = *a++;
		if(c=='\n' || n==0 || lp==&line[sizeof(line)-1])
			break;
	}
	*lp = 0;
	lights(strtoul(line, 0, 0));
}

void
consnoise(char *a, int n)
{
	int freq;
	int duration;
	char line[128];
	char *lp;
	int c;

	lp = line;
	while(n--){
		*lp++ = c = *a++;
		if(c=='\n' || n==0 || lp==&line[sizeof(line)-1]){
			*lp = 0;
			freq = strtoul(line, &lp, 0);
			while(*lp==' ' || *lp=='\t')
				lp++;
			duration = strtoul(lp, &lp, 0);
			buzz(freq, duration);
			lp = line;
		}
	}
}

long
conswrite(Chan *c, void *va, long n, ulong offset)
{
	char cbuf[64];
	char buf[256];
	long l, bp;
	char *a = va;
	Mach *mp;
	int id, fd;
	Chan *swc;

	switch(c->qid.path){
	case Qcons:
		/*
		 * Can't page fault in putstrn, so copy the data locally.
		 */
		l = n;
		while(l > 0){
			bp = l;
			if(bp > sizeof buf)
				bp = sizeof buf;
			memmove(buf, a, bp);
			putstrn(a, bp);
			a += bp;
			l -= bp;
		}
		break;

	case Qconsctl:
		if(n >= sizeof(buf))
			n = sizeof(buf)-1;
		strncpy(buf, a, n);
		buf[n] = 0;
		for(a = buf; a;){
			if(strncmp(a, "rawon", 5) == 0){
				qlock(&kbd);
				if(kbd.x){
					qwrite(kbdq, kbd.line, kbd.x, 1);
					kbd.x = 0;
				}
				kbd.raw = 1;
				qunlock(&kbd);
			} else if(strncmp(a, "rawoff", 6) == 0){
				kbd.raw = 0;
				kbd.x = 0;
			}
			if(a = strchr(a, ' '))
				a++;
		}
		break;

	case Qtime:
		if(n<=0 || boottime!=0)	/* write once file */
			return 0;
		if(n >= sizeof cbuf)
			n = sizeof cbuf - 1;
		memmove(cbuf, a, n);
		cbuf[n-1] = 0;
		boottime = strtoul(a, 0, 0)-TK2SEC(MACHP(0)->ticks);
		break;

	case Qkey:
		return keywrite(a, n);

	case Qhostowner:
		return hostownerwrite(a, n);

	case Qhostdomain:
		return hostdomainwrite(a, n);

	case Quser:
		return userwrite(a, n);

	case Qauth:
		return authwrite(c, a, n);

	case Qauthcheck:
		return authcheck(c, a, n);

	case Qauthent:
		return authentwrite(c, a, n);

	case Qnull:
		break;

	case Qnoise:
		consnoise(a, n);
		break;

	case Qlights:
		conslights(a, n);
		break;

	case Qsysstat:
		for(id = 0; id < 32; id++) {
			if(active.machs & (1<<id)) {
				mp = MACHP(id);
				mp->cs = 0;
				mp->intr = 0;
				mp->syscall = 0;
				mp->pfault = 0;
				mp->tlbfault = 0;
				mp->tlbpurge = 0;
			}
		}
		break;

	case Qswap:
		if(n >= sizeof buf)
			error(Egreg);
		memmove(buf, va, n);	/* so we can NUL-terminate */
		buf[n] = 0;
		/* start a pager if not already started */
		if(strncmp(buf, "start", 5) == 0){
			kickpager();
			break;
		}
		if(cpuserver && !iseve())
			error(Eperm);
		if(buf[0]<'0' || '9'<buf[0])
			error(Ebadarg);
		fd = strtoul(buf, 0, 0);
		swc = fdtochan(fd, -1, 1, 0);
		setswapchan(swc);
		break;

	case Qsysname:
		if(offset != 0)
			error(Ebadarg);
		if(n <= 0 || n >= NAMELEN)
			error(Ebadarg);
		strncpy(sysname, a, n);
		sysname[n] = 0;
		if(sysname[n-1] == '\n')
			sysname[n-1] = 0;
		break;

	default:
		print("conswrite: %d\n", c->qid.path);
		error(Egreg);
	}
	return n;
}

void
consremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
conswstat(Chan *c, char *dp)
{
	USED(c, dp);
	error(Eperm);
}

int
nrand(int n)
{
	static ulong randn;

	randn = randn*1103515245 + 12345 + MACHP(0)->ticks;
	return (randn>>16) % n;
}

void
setterm(char *f)
{
	char buf[2*NAMELEN];

	sprint(buf, f, conffile);
	ksetenv("terminal", buf);
}
