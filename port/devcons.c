#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#include	"devtab.h"

static struct
{
	Lock;
	uchar	buf[4000];
	uchar	*in;
	uchar	*out;
	int	printing;
	int	c;
}printq;

typedef struct IOQ	IOQ;

#define	NQ	4096
struct IOQ{
	union{
		Lock;
		QLock;
	};
	uchar	buf[NQ];
	uchar	*in;
	uchar	*out;
	int	state;
	Rendez	r;
};

IOQ	kbdq;		/* qlock to getc; interrupt putc's */
IOQ	lineq;		/* lock to getc; interrupt putc's */

void
printinit(void)
{

	printq.in = printq.buf;
	printq.out = printq.buf;
	lock(&printq);		/* allocate lock */
	unlock(&printq);

	kbdq.in = kbdq.buf;
	kbdq.out = kbdq.buf;
	lineq.in = lineq.buf;
	lineq.out = lineq.buf;
	qlock(&kbdq);		/* allocate qlock */
	qunlock(&kbdq);
	lock(&lineq);		/* allocate lock */
	unlock(&lineq);

	duartinit();
}

/*
 * Put a string on the console.
 * n bytes of s are guaranteed to fit in the buffer and is ready to print.
 * Must be called splhi() and with printq locked.
 */
void
puts(char *s, int n)
{
	if(!printq.printing){
		printq.printing = 1;
		printq.c = *s++;
		n--;
	}
	memcpy(printq.in, s, n);
	printq.in += n;
	if(printq.in >= printq.buf+sizeof(printq.buf))
		printq.in = printq.buf;
}

/*
 * Print a string on the console.  This is the high level routine
 * with a queue to the interrupt handler.  BUG: There is no check against
 * overflow.
 */
void
putstrn(char *str, int n)
{
	int s, c, m;
	char *t;

	s = splhi();
	lock(&printq);
	while(n > 0){
		if(*str == '\n')
			puts("\r", 1);
		m = printq.buf+sizeof(printq.buf) - printq.in;
		if(n < m)
			m = n;
		t = memchr(str+1, '\n', m-1);
		if(t)
			if(t-str < m)
				m = t - str;
		puts(str, m);
		n -= m;
		str += m;
	}
	unlock(&printq);
	splx(s);
}

int
cangetc(IOQ *q)
{
	return q->in != q->out;
}

int
isbrkc(IOQ *q)
{
	uchar *p;

	for(p=q->out; p!=q->in; ){
		if(*p==0x04 || *p=='\n')
			return 1;
		p++;
		if(p >= q->buf+sizeof(q->buf))
			p = q->buf;
	}
	return 0;
}

int
getc(IOQ *q)
{
	int c;

	if(q->in == q->out)
		return -1;
	c = *q->out++;
	if(q->out == q->buf+sizeof(q->buf))
		q->out = q->buf;
	return c;
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

void
panic(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	strcpy(buf, "panic: ");
	n = doprint(buf+7, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	buf[n] = '\n';
	putstrn(buf, n+1);
	exit();
}

int
pprint(char *fmt, ...)
{
	char buf[2*PRINTSIZE];
	Chan *c;
	int n;

	c = u->fd[2];
	if(c==0 || (c->mode!=OWRITE && c->mode!=ORDWR))
		return 0;
	n = sprint(buf, "%s %d: ", u->p->text, u->p->pid);
	n = doprint(buf+n, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	qlock(c);
	if(waserror()){
		qunlock(c);
		return 0;
	}
	(*devtab[c->type].write)(c, buf, n);
	c->offset += n;
	qunlock(c);
	poperror();
	return n;
}

void
prflush(void)
{
	while(printq.printing)
		delay(100);
}

/*
 * Get character to print at interrupt time.
 * Always called splhi from proc 0.
 */
int
conschar(void)
{
	uchar *p;
	int c;

	lock(&printq);
	p = printq.out;
	if(p == printq.in){
		printq.printing = 0;
		c = -1;
	}else{
		c = *p++;
		if(p >= printq.buf+sizeof(printq.buf))
			p = printq.buf;
		printq.out = p;
	}
	unlock(&printq);
	return c;
}

void
echo(int c)
{
	char ch;

	/*
	 * ^t hack BUG
	 */
	if(c == 0x14)
		DEBUG();
	if(c == 0x15)
		putstrn("^U\n", 3);
	if(c == 0x16)
		dumpqueues();
	else{
		ch = c;
		putstrn(&ch, 1);
	}
}

/*
 * Put character into read queue at interrupt time.
 * Always called splhi from proc 0.
 */
void
kbdchar(int c)
{
	if(c == '\r')
		c = '\n';
	echo(c);
	*kbdq.in++ = c;
	if(kbdq.in == kbdq.buf+sizeof(kbdq.buf))
		kbdq.in = kbdq.buf;
	if(c=='\n' || c==0x04)
		wakeup(&kbdq.r);
}

void
printslave(void)
{
	int c;

	c = printq.c;
	if(c){
		printq.c = 0;
		duartxmit(c);
	}
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
	Qnull,
	Qpgrpid,
	Qpid,
	Qppid,
	Qtime,
	Quser,
};

Dirtab consdir[]={
	"cons",		Qcons,		0,	0600,
	"cputime",	Qcputime,	72,	0600,
	"null",		Qnull,		0,	0600,
	"pgrpid",	Qpgrpid,	12,	0600,
	"pid",		Qpid,		12,	0600,
	"ppid",		Qppid,		12,	0600,
	"time",		Qtime,		12,	0600,
	"user",		Quser,		0,	0600,
};

#define	NCONS	(sizeof consdir/sizeof(Dirtab))

ulong	boottime;		/* seconds since epoch at boot */

long
seconds(void)
{
	return boottime + TK2MS(MACHP(0)->ticks);
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
	memcpy(buf, tmp+off, n);
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
	memcpy(buf, str+off, n);
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
	if(c->qid==Quser && omode==(OWRITE|OTRUNC)){
		/* truncate? */
		if(strcmp(u->p->pgrp->user, "bootes") == 0)	/* BUG */
			u->p->pgrp->user[0] = 0;
		else
			error(0, Eperm);
	}
	return devopen(c, omode, consdir, NCONS, devgen);
}

void
conscreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void
consclose(Chan *c)
{
}

long
consread(Chan *c, void *buf, long n)
{
	int ch, i, j, k;
	ulong l;
	uchar *out;
	char *cbuf = buf;
	char *user;
	int userlen;
	char tmp[6*NUMSIZE];

	if(n <= 0)
		return n;
	switch(c->qid&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, consdir, NCONS, devgen);

	case Qcons:
		qlock(&kbdq);
		while(!cangetc(&lineq)){
			sleep(&kbdq.r, (int(*)(void*))isbrkc, &kbdq);
			do{
				ch = getc(&kbdq);
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
				default:
					*lineq.in++ = ch;
					if(lineq.in == lineq.buf+sizeof(lineq.buf))
					lineq.in = lineq.buf;
				}
			}while(ch!='\n' && ch!=0x04);
		}
		i = 0;
		while(n>0){
			ch = getc(&lineq);
			if(ch == 0x04 || ch == -1)
				break;
			i++;
			*cbuf++ = ch;
			--n;
		}
		qunlock(&kbdq);
		return i;

	case Qcputime:
		k = c->offset;
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
		memcpy(buf, tmp+k, n);
		return n;

	case Qpgrpid:
		return readnum(c->offset, buf, n, u->p->pgrp->pgrpid, NUMSIZE);

	case Qpid:
		return readnum(c->offset, buf, n, u->p->pid, NUMSIZE);

	case Qppid:
		return readnum(c->offset, buf, n, u->p->parentpid, NUMSIZE);

	case Qtime:
		return readnum(c->offset, buf, n, boottime+TK2MS(MACHP(0)->ticks), 12);

	case Quser:
		return readstr(c->offset, buf, n, u->p->pgrp->user);

	case Qnull:
		return 0;

	default:
		panic("consread %lux\n", c->qid);
		return 0;
	}
}

long
conswrite(Chan *c, void *va, long n)
{
	char cbuf[64];
	char buf[256];
	long l, m;
	char *a = va;

	switch(c->qid){
	case Qcons:
		/*
		 * Damn. Can't page fault in putstrn, so copy the data locally.
		 */
		l = n;
		while(l > 0){
			m = l;
			if(m > sizeof buf)
				m = sizeof buf;
			memcpy(buf, a, m);
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
		memcpy(cbuf, a, n);
		cbuf[n-1] = 0;
		boottime = strtoul(a, 0, 0);
		break;

	case Quser:
		if(u->p->pgrp->user[0])		/* trying to overwrite /dev/user */
			error(0, Eperm);
		if(c->offset >= NAMELEN-1)
			return 0;
		if(c->offset+n >= NAMELEN-1)
			n = NAMELEN-1 - c->offset;
		memcpy(u->p->pgrp->user+c->offset, a, n);
		u->p->pgrp->user[c->offset+n] = 0;
		break;

	case Qcputime:
	case Qpgrpid:
	case Qpid:
	case Qppid:
		error(0, Eperm);

	case Qnull:
		break;
	default:
		error(0, Egreg);
	}
	return n;
}

void
consremove(Chan *c)
{
	error(0, Eperm);
}

void
conswstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
conserrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}

void
consuserstr(Error *e, char *buf)
{
	strcpy(buf, u->p->pgrp->user);
}
