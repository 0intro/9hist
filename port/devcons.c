#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#include	"devtab.h"

struct {
	IOQ;			/* lock to klogputs */
	QLock;			/* qlock to getc */
}	klogq;

IOQ	lineq;			/* lock to getc; interrupt putc's */
IOQ	printq;
IOQ	mouseq;
KIOQ	kbdq;

Ref	raw;		/* whether kbd i/o is raw (rcons is open) */

/*
 *  init the queues and set the output routine
 */
void
printinit(void)
{
	initq(&printq);
	printq.puts = 0;
	initq(&lineq);
	initq(&kbdq);
	kbdq.putc = kbdputc;
	initq(&klogq);
	initq(&mouseq);
	mouseq.putc = mouseputc;
}

/*
 *   Print a string on the console.  Convert \n to \r\n
 */
void
putstrn(char *str, int n)
{
	int s, c, m;
	char *t;

	while(n > 0){
		if(printq.puts && *str=='\n')
			(*printq.puts)(&printq, "\r", 1);
		m = n;
		t = memchr(str+1, '\n', m-1);
		if(t)
			if(t-str < m)
				m = t - str;
		if(printq.puts)
			(*printq.puts)(&printq, str, m);
		screenputs(str, m);
		n -= m;
		str += m;
	}
}

/*
 *   Print a string in the kernel log.  Ignore overflow.
 */
void
klogputs(char *str, long n)
{
	int s, m;
	uchar *nextin;

	s = splhi();
	lock(&klogq);
	while(n){
		m = &klogq.buf[NQ] - klogq.in;
		if(m > n)
			m = n;
		memmove(klogq.in, str, m);
		n -= m;
		str += m;
		nextin = klogq.in + m;
		if(nextin >= &klogq.buf[NQ])
			klogq.in = klogq.buf;
		else
			klogq.in = nextin;
	}
	unlock(&klogq);
	splx(s);
	wakeup(&klogq.r);
}

int
isbrkc(KIOQ *q)
{
	uchar *p;

	for(p=q->out; p!=q->in; ){
		if(raw.ref)
			return 1;
		if(*p==0x04 || *p=='\n')
			return 1;
		p++;
		if(p >= q->buf+sizeof(q->buf))
			p = q->buf;
	}
	return 0;
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
	klogputs(buf, n);
	return n;
}

void
panic(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	strcpy(buf, "panic: ");
	n = doprint(buf+7, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	buf[n] = '\n';
	putstrn(buf, n+1);
	dumpstack();
	if(conf.cntrlp)
		exit();
	else
		for(;;);
}

int
pprint(char *fmt, ...)
{
	char buf[2*PRINTSIZE];
	Chan *c;
	int n;

	if(u->p->fgrp == 0)
		return 0;

	c = u->p->fgrp->fd[2];
	if(c==0 || (c->mode!=OWRITE && c->mode!=ORDWR))
		return 0;
	n = sprint(buf, "%s %d: ", u->p->text, u->p->pid);
	n = doprint(buf+n, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	qlock(&c->wrl);
	if(waserror()){
		qunlock(&c->wrl);
		return 0;
	}
	(*devtab[c->type].write)(c, buf, n, c->offset);
	c->offset += n;
	qunlock(&c->wrl);
	poperror();
	return n;
}

void
prflush(void)
{
	while(printq.in != printq.out) ;
}

void
echo(int c)
{
	char ch;
	static int ctrlt;

	/*
	 * ^p hack
	 */
	if(c==0x10 && conf.cntrlp)
		panic("^p");

	/*
	 * ^t hack BUG
	 */
	if(ctrlt == 2){
		ctrlt = 0;
		switch(c){
		case 0x14:
			break;	/* pass it on */
		case 'm':
			mntdump();
			return;
		case 'p':
			procdump();
			return;
		case 'q':
			dumpqueues();
			return;
		case 'r':
			exit();
			break;
		}
	}else if(c == 0x14){
		ctrlt++;
		return;
	}
	ctrlt = 0;
	if(raw.ref)
		return;
	if(c == 0x15)
		putstrn("^U\n", 3);
	else{
		ch = c;
		putstrn(&ch, 1);
	}
}

/*
 * Put character into read queue at interrupt time.
 * Always called splhi from proc 0.
 */
int
kbdputc(IOQ *q, int ch)
{
	echo(ch);
	kbdq.c = ch;
	*kbdq.in++ = ch;
	if(kbdq.in == kbdq.buf+sizeof(kbdq.buf))
		kbdq.in = kbdq.buf;
	if(raw.ref || ch=='\n' || ch==0x04)
		wakeup(&kbdq.r);
	return 0;
}

void
kbdrepeat(int rep)
{
	kbdq.repeat = rep;
	kbdq.count = 0;
}

void
kbdclock(void)
{
	if(kbdq.repeat == 0)
		return;
	if(kbdq.repeat==1 && ++kbdq.count>HZ){
		kbdq.repeat = 2;
		kbdq.count = 0;
		return;
	}
	if(++kbdq.count&1)
		kbdputc(&kbdq, kbdq.c);
}

int
consactive(void)
{
	return printq.in != printq.out;
}

/*
 * I/O interface
 */
enum{
	Qdir,
	Qcons,
	Qcputime,
	Qlights,
	Qnoise,
	Qnull,
	Qpgrpid,
	Qpid,
	Qppid,
	Qrcons,
	Qtime,
	Quser,
	Qklog,
	Qmsec,
	Qclock,
	Qsysstat,
	Qswap,
};

Dirtab consdir[]={
	"cons",		{Qcons},	0,		0600,
	"cputime",	{Qcputime},	6*NUMSIZE,	0600,
	"lights",	{Qlights},	0,		0600,
	"noise",	{Qnoise},	0,		0600,
	"null",		{Qnull},	0,		0600,
	"pgrpid",	{Qpgrpid},	NUMSIZE,	0600,
	"pid",		{Qpid},		NUMSIZE,	0600,
	"ppid",		{Qppid},	NUMSIZE,	0600,
	"rcons",	{Qrcons},	0,		0600,
	"time",		{Qtime},	NUMSIZE,	0600,
	"user",		{Quser},	0,		0600,
	"klog",		{Qklog},	0,		0400,
	"msec",		{Qmsec},	NUMSIZE,	0400,
	"clock",	{Qclock},	2*NUMSIZE,	0400,
	"sysstat",	{Qsysstat},	0,		0600,
	"swap",		{Qswap},	0,		0666,
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
	Op op = (Op){ tmp, tmp+sizeof(tmp), &val, size-1, 0, FUNSIGN|FLONG };

	numbconv(&op, 10);
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
	int ch;

	switch(c->qid.path){
	case Quser:
		if(omode==(OWRITE|OTRUNC)){
			/* truncate? */
			if(strcmp(u->p->pgrp->user, "bootes") == 0)	/* BUG */
				u->p->pgrp->user[0] = 0;
			else
				error(Eperm);
		}
		break;
	case Qrcons:
		if(conf.cntrlp)
			error(Eperm);
		if(incref(&raw) == 1){
			lock(&lineq);
			while((ch=getc(&kbdq)) != -1){
				*lineq.in++ = ch;
				if(lineq.in == lineq.buf+sizeof(lineq.buf))
					lineq.in = lineq.buf;
			}
			unlock(&lineq);
		}
		break;
	case Qswap:
		kickpager();		/* start a pager if not already started */
		break;
	}
	return devopen(c, omode, consdir, NCONS, devgen);
}

void
conscreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void
consclose(Chan *c)
{
	if(c->qid.path==Qrcons && (c->flag&COPEN))
		decref(&raw);
}


long
consread(Chan *c, void *buf, long n, ulong offset)
{
	int ch, i, j, k, id;
	ulong l;
	uchar *out;
	char *cbuf = buf;
	char *user;
	int userlen;
	char tmp[6*NUMSIZE], xbuf[1024];
	Mach *mp;

	if(n <= 0)
		return n;
	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, consdir, NCONS, devgen);

	case Qrcons:
	case Qcons:
		qlock(&kbdq);
		if(waserror()){
			qunlock(&kbdq);
			nexterror();
		}
		while(!cangetc(&lineq)){
			sleep(&kbdq.r, isbrkc, &kbdq);
			do{
				lock(&lineq);
				ch = getc(&kbdq);
				if(raw.ref){
					unlock(&lineq);
					goto Default;
				}
				switch(ch){
				case '\b':
					if(lineq.in != lineq.out){
						if(lineq.in == lineq.buf)
							lineq.in = lineq.buf+sizeof(lineq.buf);
						lineq.in--;
					}
					break;
				case 0x15:
					lineq.in = lineq.out;
					break;
				Default:
				default:
					*lineq.in = ch;
					if(lineq.in >= lineq.buf+sizeof(lineq.buf)-1)
						lineq.in = lineq.buf;
					else
						lineq.in++;
				}
				unlock(&lineq);
			}while(raw.ref==0 && ch!='\n' && ch!=0x04);
		}
		i = 0;
		while(n > 0){
			ch = getc(&lineq);
			if(ch==-1 || (raw.ref==0 && ch==0x04))
				break;
			i++;
			*cbuf++ = ch;
			--n;
		}
		poperror();
		qunlock(&kbdq);
		return i;

	case Qcputime:
		k = offset;
		if(k >= sizeof tmp)
			return 0;
		if(k+n > sizeof tmp)
			n = sizeof tmp - k;
		/* easiest to format in a separate buffer and copy out */
		for(i=0; i<6 && NUMSIZE*i<k+n; i++){
			l = u->p->time[i];
			if(i == TReal)
				l = MACHP(0)->ticks - l;
			l = TK2MS(l);
			readnum(0, tmp+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		memmove(buf, tmp+k, n);
		return n;

	case Qpgrpid:
		return readnum(offset, buf, n, u->p->pgrp->pgrpid, NUMSIZE);

	case Qpid:
		return readnum(offset, buf, n, u->p->pid, NUMSIZE);

	case Qppid:
		return readnum(offset, buf, n, u->p->parentpid, NUMSIZE);

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

	case Quser:
		return readstr(offset, buf, n, u->p->pgrp->user);

	case Qnull:
		return 0;

	case Qklog:
		qlock(&klogq);
		if(waserror()){
			qunlock(&klogq);
			nexterror();
		}
		while(!cangetc(&klogq))
			sleep(&klogq.r, cangetc, &klogq);
		for(i=0; i<n; i++){
			if((ch=getc(&klogq)) == -1)
				break;
			*cbuf++ = ch;
		}
		poperror();
		qunlock(&klogq);
		return i;

	case Qmsec:
		return readnum(offset, buf, n, TK2MS(MACHP(0)->ticks), NUMSIZE);

	case Qsysstat:
		j = 0;
		for(id = 0; id < 32; id++) {
			if(active.machs & (1<<id)) {
				mp = MACHP(id);
				j += sprint(&xbuf[j], "%d %d %d %d %d %d %d %d\n",
					id, mp->cs, mp->intr, mp->syscall, mp->pfault,
					    mp->tlbfault, mp->tlbpurge, m->spinlock);
			}
		}
		return readstr(offset, buf, n, xbuf);

	case Qswap:
		sprint(xbuf, "%d/%d memory %d/%d swap\n",
				palloc.user-palloc.freecount, palloc.user, 
				conf.nswap-swapalloc.free, conf.nswap);

		return readstr(offset, buf, n, xbuf);
	default:
		panic("consread %lux\n", c->qid);
		return 0;
	}
}

void
conslights(char *a, int n)
{
	int l;
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
	long l, m;
	char *a = va;
	Mach *mp;
	int id, fd;
	Chan *swc;

	switch(c->qid.path){
	case Qrcons:
	case Qcons:
		/*
		 * Damn. Can't page fault in putstrn, so copy the data locally.
		 */
		l = n;
		while(l > 0){
			m = l;
			if(m > sizeof buf)
				m = sizeof buf;
			memmove(buf, a, m);
			putstrn(a, m);
			a += m;
			l -= m;
		}
		break;

	case Qtime:
		if(n<=0 || boottime!=0)	/* only one write please */
			return 0;
		if(n >= sizeof cbuf)
			n = sizeof cbuf - 1;
		memmove(cbuf, a, n);
		cbuf[n-1] = 0;
		boottime = strtoul(a, 0, 0);
		break;

	case Quser:
		if(u->p->pgrp->user[0])		/* trying to overwrite /dev/user */
			error(Eperm);
		if(offset >= NAMELEN-1)
			return 0;
		if(offset+n >= NAMELEN-1)
			n = NAMELEN-1 - offset;
		memmove(u->p->pgrp->user+offset, a, n);
		u->p->pgrp->user[offset+n] = 0;
		break;

	case Qcputime:
	case Qpgrpid:
	case Qpid:
	case Qppid:
		error(Eperm);

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
				mp->spinlock = 0;
			}
		}
		break;

	case Qswap:
		if(n >= sizeof buf)
			error(Egreg);
		memmove(buf, va, n);	/* so we can NUL-terminate */
		buf[n] = 0;
		fd = strtoul(buf, 0, 0);
		swc = fdtochan(fd, -1);
		setswapchan(swc);
		return n;

	default:
		error(Egreg);
	}
	return n;
}

void
consremove(Chan *c)
{
	error(Eperm);
}

void
conswstat(Chan *c, char *dp)
{
	error(Eperm);
}
