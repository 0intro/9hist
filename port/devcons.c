#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"pool.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

void	(*consdebug)(void);

Queue*	kbdq;			/* unprocessed console input */
Queue*	lineq;			/* processed console input */
Queue*	printq;			/* console output */

static struct
{
	QLock;

	int	raw;		/* true if we shouldn't process input */
	int	ctl;		/* number of opens to the control file */
	int	x;		/* index into line */
	char	line[1024];	/* current input line */

	Rune	c;
	int	count;
	int	repeat;
	int	ctlpoff;
} kbd;

char	sysname[NAMELEN];

static ulong	randomread(void*, ulong);
static void	randominit(void);
static void	seednrand(void);
static long	qtimer(long, vlong);

int qtimerentry = -1;
vlong µoffset;

void
printinit(void)
{
	lineq = qopen(2*1024, 0, 0, 0);
	if(lineq == nil)
		panic("printinit");
	qnoblock(lineq, 1);
}

int
consactive(void)
{
	if(printq)
		return qlen(printq) > 0;
	return 0;
}

void
prflush(void)
{
	while(consactive())
		;
}

/*
 *   Print a string on the console.  Convert \n to \r\n for serial
 *   line consoles.  Locking of the queues is left up to the screen
 *   or uart code.  Multi-line messages to serial consoles may get
 *   interspersed with other messages.
 */
static void
putstrn0(char *str, int n, int usewrite)
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
			if(usewrite)
				qwrite(printq, buf, m+2);
			else
				qiwrite(printq, buf, m+2);
			str = t + 1;
			n -= m + 1;
		} else {
			if(usewrite)
				qwrite(printq, str, n);
			else
				qiwrite(printq, str, n);
			break;
		}
	}
}

void
putstrn(char *str, int n)
{
	putstrn0(str, n, 0);
}

int
snprint(char *s, int n, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	n = doprint(s, s+n, fmt, arg) - s;
	va_end(arg);
	return n;
}

int
sprint(char *s, char *fmt, ...)
{
	int n;
	va_list arg;

	va_start(arg, fmt);
	n = doprint(s, s+PRINTSIZE, fmt, arg) - s;
	va_end(arg);
	return n;
}

int
print(char *fmt, ...)
{
	int n;
	va_list arg;
	char buf[PRINTSIZE];

	va_start(arg, fmt);
	n = doprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);
	putstrn(buf, n);

	return n;
}

void
panic(char *fmt, ...)
{
	int n;
	va_list arg;
	char buf[PRINTSIZE];

	strcpy(buf, "panic: ");
	va_start(arg, fmt);
	n = doprint(buf+strlen(buf), buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);
	buf[n] = '\n';
	putstrn(buf, n+1);
	spllo();
	prflush();
	dumpstack();

	exit(1);
}

int
pprint(char *fmt, ...)
{
	int n;
	Chan *c;
	va_list arg;
	char buf[2*PRINTSIZE];

	if(up->fgrp == 0)
		return 0;

	c = up->fgrp->fd[2];
	if(c==0 || (c->mode!=OWRITE && c->mode!=ORDWR))
		return 0;
	n = sprint(buf, "%s %d: ", up->text, up->pid);
	va_start(arg, fmt);
	n = doprint(buf+n, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	if(waserror())
		return 0;
	devtab[c->type]->write(c, buf, n, c->offset);
	poperror();

	lock(c);
	c->offset += n;
	unlock(c);

	return n;
}

void
echo(Rune r, char *buf, int n)
{
	static int ctrlt, pid;
	extern ulong etext;

	/*
	 * ^p hack
	 */
	if(r==0x10 && cpuserver && !kbd.ctlpoff){
		lock(&active);
		active.exiting = 1;
		unlock(&active);
	}

	/*
	 * ^t hack BUG
	 */
	if(ctrlt == 2){
		ctrlt = 0;
		switch(r){
		case 0x14:
			break;	/* pass it on */
		case 's':
			dumpstack();
			break;
		case 'x':
			xsummary();
			ixsummary();
			poolsummary();
			break;
		case 'd':
			if(consdebug != nil)
				consdebug();
			return;
		case 'p':
			procdump();
			return;
		case 'q':
			scheddump();
			break;
		case 'k':
			if(!cpuserver)
				killbig();
			break;
		case 'r':
			exit(0);
			break;
		}
	}
	else if(r == 0x14){
		ctrlt++;
		return;
	}
	ctrlt = 0;
	if(kbd.raw)
		return;

	/*
	 *  finally, the actual echoing
	 */
	if(r == '\n'){
		if(printq)
			qiwrite(printq, "\r", 1);
	} else if(r == 0x15){
		buf = "^U\n";
		n = 3;
	}
	screenputs(buf, n);
	if(printq)
		qiwrite(printq, buf, n);
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
kbdputc(Queue*, int ch)
{
	int n;
	char buf[3];
	Rune r;

	r = ch;
	n = runetochar(buf, &r);
	if(n == 0)
		return 0;
	echo(r, buf, n);
	kbd.c = r;
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

enum{
	Qdir,
	Qauth,
	Qauthcheck,
	Qauthent,
	Qclock,
	Qcons,
	Qconsctl,
	Qcputime,
	Qdrivers,
	Qhz,
	Qkey,
	Qhostdomain,
	Qhostowner,
	Qmsec,
	Qnull,
	Qpgrpid,
	Qpid,
	Qppid,
	Qrandom,
	Qreboot,
	Qswap,
	Qsysname,
	Qsysstat,
	Qtime,
	Qtimer,
	Quser,
};

static Dirtab consdir[]={
	"authenticate",	{Qauth},	0,		0666,
	"authcheck",	{Qauthcheck},	0,		0666,
	"authenticator", {Qauthent},	0,		0666,
	"clock",	{Qclock},	2*NUMSIZE,	0444,
	"cons",		{Qcons},	0,		0660,
	"consctl",	{Qconsctl},	0,		0220,
	"cputime",	{Qcputime},	6*NUMSIZE,	0444,
	"drivers",	{Qdrivers},	0,		0644,
	"hostdomain",	{Qhostdomain},	DOMLEN,		0664,
	"hostowner",	{Qhostowner},	NAMELEN,	0664,
	"hz",		{Qhz},		NUMSIZE,	0666,
	"key",		{Qkey},		DESKEYLEN,	0622,
	"msec",		{Qmsec},	NUMSIZE,	0444,
	"null",		{Qnull},	0,		0666,
	"pgrpid",	{Qpgrpid},	NUMSIZE,	0444,
	"pid",		{Qpid},		NUMSIZE,	0444,
	"ppid",		{Qppid},	NUMSIZE,	0444,
	"random",	{Qrandom},	0,		0664,
	"reboot",	{Qreboot},	0,		0664,
	"swap",		{Qswap},	0,		0664,
	"sysname",	{Qsysname},	0,		0664,
	"sysstat",	{Qsysstat},	0,		0666,
	"time",		{Qtime},	NUMSIZE,	0664,
	"timer",	{Qtimer},	0,		0444,
 	"user",		{Quser},	NAMELEN,	0666,
};

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

	snprint(tmp, sizeof(tmp), "%*.0ud", size-1, val);
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

static void
consinit(void)
{
	randominit();

	if (qtimerentry < 0) {
		while (consdir[++qtimerentry].qid.path != Qtimer)
			;
		µoffset = (vlong)1000000*rtctime() -
			  (vlong)1000*TK2MS(MACHP(0)->ticks);
	}
}

static Chan*
consattach(char *spec)
{
	static int seeded;

	if(!seeded){
		seednrand();
		seeded = 1;
	}
	return devattach('c', spec);
}

static int
conswalk(Chan *c, char *name)
{
	return devwalk(c, name, consdir, nelem(consdir), devgen);
}

static void
consstat(Chan *c, char *dp)
{
	if (c->qid.path == Qtimer)
		consdir[qtimerentry].length = µoffset +
			(vlong)1000*TK2MS(MACHP(0)->ticks);
	devstat(c, dp, consdir, nelem(consdir), devgen);
}

static Chan*
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
	return devopen(c, omode, consdir, nelem(consdir), devgen);
}

static void
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
		break;
	case Qauth:
	case Qauthcheck:
	case Qauthent:
		authclose(c);
	}
}

static long
consread(Chan *c, void *buf, long n, vlong off)
{
	ulong l;
	Mach *mp;
	char *b, *bp;
	char tmp[128];		/* must be >= 6*NUMSIZE */
	char *cbuf = buf;
	int ch, i, k, id, eol;
	vlong offset = off;

	if(n <= 0)
		return n;
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		consdir[qtimerentry].length = µoffset +
			(vlong)1000*TK2MS(MACHP(0)->ticks);
		return devdirread(c, buf, n, consdir, nelem(consdir), devgen);

	case Qcons:
		qlock(&kbd);
		if(waserror()) {
			qunlock(&kbd);
			nexterror();
		}
		if(kbd.raw) {
			if(qcanread(lineq))
				n = qread(lineq, buf, n);
			else {
				/* read as much as possible */
				do {
					i = qread(kbdq, cbuf, n);
					cbuf += i;
					n -= i;
				} while (n>0 && qcanread(kbdq));
				n = cbuf - (char*)buf;
			}
		} else {
			while(!qcanread(lineq)) {
				qread(kbdq, &kbd.line[kbd.x], 1);
				ch = kbd.line[kbd.x];
				if(kbd.raw){
					qiwrite(lineq, kbd.line, kbd.x+1);
					kbd.x = 0;
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
					qwrite(lineq, kbd.line, kbd.x);
					kbd.x = 0;
				}
			}
			n = qread(lineq, buf, n);
		}
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
		return readnum((ulong)offset, buf, n, up->pgrp->pgrpid, NUMSIZE);

	case Qpid:
		return readnum((ulong)offset, buf, n, up->pid, NUMSIZE);

	case Qppid:
		return readnum((ulong)offset, buf, n, up->parentpid, NUMSIZE);

	case Qtime:
		return readnum((ulong)offset, buf, n, rtctime(), 12);

	case Qtimer:
		return qtimer(n, offset);

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

	case Qauthcheck:
		return authcheckread(c, cbuf, n);

	case Qauthent:
		return authentread(c, cbuf, n);

	case Qhostowner:
		return readstr((ulong)offset, buf, n, eve);

	case Qhostdomain:
		return readstr((ulong)offset, buf, n, hostdomain);

	case Quser:
		return readstr((ulong)offset, buf, n, up->user);

	case Qnull:
		return 0;

	case Qmsec:
		return readnum((ulong)offset, buf, n, TK2MS(MACHP(0)->ticks), NUMSIZE);

	case Qhz:
		return readnum((ulong)offset, buf, n, HZ, NUMSIZE);

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
		n = readstr((ulong)offset, buf, n, b);
		free(b);
		return n;

	case Qswap:
		sprint(tmp, "%d/%d memory %d/%d swap\n",
			palloc.user-palloc.freecount,
			palloc.user, conf.nswap-swapalloc.free, conf.nswap);

		return readstr((ulong)offset, buf, n, tmp);

	case Qsysname:
		return readstr((ulong)offset, buf, n, sysname);

	case Qrandom:
		return randomread(buf, n);

	case Qdrivers:
		b = malloc(READSTR);
		if(b == nil)
			error(Enomem);
		n = 0;
		for(i = 0; devtab[i] != nil; i++)
			n += snprint(b+n, READSTR-n, "#%C %s\n", devtab[i]->dc,  devtab[i]->name);
		n = readstr((ulong)offset, buf, n, b);
		free(b);
		return n;

	default:
		print("consread %lux\n", c->qid);
		error(Egreg);
	}
	return -1;		/* never reached */
}

static long
conswrite(Chan *c, void *va, long n, vlong off)
{
	char cbuf[64];
	char buf[256];
	long l, bp;
	char *a = va;
	Mach *mp;
	int id, fd;
	Chan *swc;
	ulong offset = off;

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
			putstrn0(a, bp, 1);
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
					qwrite(kbdq, kbd.line, kbd.x);
					kbd.x = 0;
				}
				kbd.raw = 1;
				qunlock(&kbd);
			} else if(strncmp(a, "rawoff", 6) == 0){
				kbd.raw = 0;
				kbd.x = 0;
			} else if(strncmp(a, "ctlpon", 6) == 0){
				kbd.ctlpoff = 0;
			} else if(strncmp(a, "ctlpoff", 7) == 0){
				kbd.ctlpoff = 1;
			}
			if(a = strchr(a, ' '))
				a++;
		}
		break;

	case Qtime:
		if(n<=0 || (boottime != 0 && !iseve()))	/* write once file */
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

	case Qreboot:
		if(!iseve())
			error(Eperm);
		if(strncmp(a, "reboot", 6) == 0){
			print("conswrite: reboot\n");
			exit(0);
		}
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
		swc = fdtochan(fd, -1, 1, 1);
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

	case Qtimer:
		error(Eperm);
		break;

	default:
		print("conswrite: %d\n", c->qid.path);
		error(Egreg);
	}
	return n;
}

void
setterm(char *f)
{
	char buf[2*NAMELEN];

	sprint(buf, f, conffile);
	ksetenv("terminal", buf);
}

Dev consdevtab = {
	'c',
	"cons",

	devreset,
	consinit,
	consattach,
	devclone,
	conswalk,
	consstat,
	consopen,
	devcreate,
	consclose,
	consread,
	devbread,
	conswrite,
	devbwrite,
	devremove,
	devwstat,
};

static struct
{
	QLock;
	Rendez	producer;
	Rendez	consumer;
	ulong	randomcount;
	uchar	buf[4096];
	uchar	*ep;
	uchar	*rp;
	uchar	*wp;
	uchar	next;
	uchar	wakeme;
	ushort	bits;
	ulong	randn;
} rb;

static void
seednrand(void)
{
	randomread((void*)&rb.randn, sizeof(rb.randn));
}

int
nrand(int n)
{
	rb.randn = rb.randn*1103515245 + 12345 + MACHP(0)->ticks;
	return (rb.randn>>16) % n;
}

static int
rbnotfull(void*)
{
	int i;

	i = rb.rp - rb.wp;
	return i != 1 && i != (1 - sizeof(rb.buf));
}

static int
rbnotempty(void*)
{
	return rb.wp != rb.rp;
}

void
genrandom(void*)
{
	up->basepri = PriNormal;
	up->priority = up->basepri;

	for(;;){
		for(;;)
			if(++rb.randomcount > 100000)
				break;
			if(anyhigher())
				sched();
		if(!rbnotfull(0))
			sleep(&rb.producer, rbnotfull, 0);
	}
}

/*
 *  produce random bits in a circular buffer
 */
static void
randomclock(void)
{
	if(rb.randomcount == 0 || !rbnotfull(0))
		return;

	rb.bits = (rb.bits<<2) ^ rb.randomcount;
	rb.randomcount = 0;

	rb.next++;
	if(rb.next != 8/2)
		return;
	rb.next = 0;

	*rb.wp ^= rb.bits;
	if(rb.wp+1 == rb.ep)
		rb.wp = rb.buf;
	else
		rb.wp = rb.wp+1;

	if(rb.wakeme)
		wakeup(&rb.consumer);
}

static void
randominit(void)
{
	addclock0link(randomclock);
	rb.ep = rb.buf + sizeof(rb.buf);
	rb.rp = rb.wp = rb.buf;
	kproc("genrandom", genrandom, 0);
}

/*
 *  consume random bytes from a circular buffer
 */
static ulong
randomread(void *xp, ulong n)
{
	uchar *e, *p;
	ulong x;

	p = xp;

	if(waserror()){
		qunlock(&rb);
		nexterror();
	}

	qlock(&rb);
	for(e = p + n; p < e; ){
		if(rb.wp == rb.rp){
			rb.wakeme = 1;
			wakeup(&rb.producer);
			sleep(&rb.consumer, rbnotempty, 0);
			rb.wakeme = 0;
			continue;
		}

		/*
		 *  beating clocks will be precictable if
		 *  they are synchronized.  Use a cheap pseudo
		 *  random number generator to obscure any cycles.
		 */
		x = rb.randn*1103515245 ^ *rb.rp;
		*p++ = rb.randn = x;

		if(rb.rp+1 == rb.ep)
			rb.rp = rb.buf;
		else
			rb.rp = rb.rp+1;
	}
	qunlock(&rb);
	poperror();

	wakeup(&rb.producer);

	return n;
}

static long
qtimer(long n, vlong offset) {
	/* block until time ≥ offset;
	 * add n to offset
	 * return increment to offset (i.e., n)
	 */
	vlong time, rathole;

	for (;;) {
		rathole = µoffset/1000;
		time = offset/1000 - (rathole + TK2MS(MACHP(0)->ticks));
		if (time <= 0) break;
		tsleep(&up->sleep, return0, 0, (long)time);
	}
	return n;
}
