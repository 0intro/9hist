#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

#include	"devtab.h"

typedef struct IOQ	IOQ;

static struct
{
	Lock;
	int	printing;
	int	c;
}printq;

#define	NQ	2048
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

IOQ	lineq;

struct{
	IOQ;
	Lock	put;
}klogq;

struct{
	IOQ;		/* qlock to getc; interrupt putc's */
	int	c;
	int	repeat;
	int	count;
}kbdq;

Ref	raw;		/* whether kbd i/o is raw (rcons is open) */

/*
 *  rs232 stream module
 */
typedef struct Rs232	Rs232;
typedef struct IOBQ	IOBQ;

#define NBQ 6
struct IOBQ{
	Block	*bp[NBQ];
	int	w;
	int	r;
	int	f;
};
#define NEXT(x) ((x+1)%NBQ)

struct Rs232{
	QLock;
	QLock	outlock;
	IOQ	in;
	IOBQ	out;
	int	kstarted;	/* true if kproc started */
	Queue	*wq;
	Alarm	*a;		/* alarm for waking the rs232 kernel process */
	int	started;
	int	delay;		/* time between character input and waking kproc */
	Rendez	r;
};

Rs232 rs232;

static void	rs232output(Rs232*);
static void	rs232input(Rs232*);
static void	rs232timer(Alarm*);
static void	rs232kproc(void*);
static void	rs232open(Queue*, Stream*);
static void	rs232close(Queue*);
static void	rs232oput(Queue*, Block*);
Qinfo rs232info =
{
	nullput,
	rs232oput,
	rs232open,
	rs232close,
	"rs232"
};

void
printinit(void)
{

	lock(&printq);		/* allocate lock */
	unlock(&printq);

	kbdq.in = kbdq.buf;
	kbdq.out = kbdq.buf;
	klogq.in = klogq.buf;
	klogq.out = klogq.buf;
	lineq.in = lineq.buf;
	lineq.out = lineq.buf;
	rs232.in.in = rs232.in.buf;
	rs232.in.out = rs232.in.buf;
	qlock(&kbdq);		/* allocate qlock */
	qunlock(&kbdq);
	lock(&lineq);		/* allocate lock */
	unlock(&lineq);
	lock(&klogq);		/* allocate lock */
	unlock(&klogq);
	lock(&klogq.put);	/* allocate lock */
	unlock(&klogq.put);

	screeninit();
}

/*
 * Print a string on the console.
 */
void
putstrn(char *str, long n)
{
	int s;

	s = splhi();
	lock(&printq);
	printq.printing = 1;
	while(--n >= 0)
		screenputc(*str++);
	printq.printing = 0;
	unlock(&printq);
	splx(s);
}

int
cangetc(void *arg)
{
	IOQ *q = (IOQ *)arg;
	int n = q->in - q->out;
	if (n < 0)
		n += sizeof(q->buf);
	return n;
}

int
canputc(void *arg)
{
	IOQ *q = (IOQ *)arg;
	return sizeof(q->buf)-cangetc(q)-1;
}

int
isbrkc(void *arg)
{
	IOQ *q = (IOQ *)arg;
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
putc(IOQ *q, int c)
{
	uchar *nextin;
	if(q->in >= &q->buf[sizeof(q->buf)-1])
		nextin = q->buf;
	else
		nextin = q->in+1;
	if(nextin == q->out)
		return -1;
	*q->in = c;
	q->in = nextin;
	return 0;
}

void
putstrk(char *str, long n)
{
	int s;

	s = splhi();
	lock(&klogq.put);
	while(--n >= 0){
		*klogq.in++ = *str++;
		if(klogq.in == klogq.buf+sizeof(klogq.buf))
			klogq.in = klogq.buf;
	}
	unlock(&klogq.put);
	splx(s);
	wakeup(&klogq.r);
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
	putstrk(buf, n);
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

void
echo(int c)
{
	char ch;
	static int ctrlt;

	/*
	 * ^t hack BUG
	 */
	if(ctrlt == 2){
		ctrlt = 0;
		switch(c){
		case 0x14:
			break;	/* pass it on */
		case 'p':
			DEBUG();
			return;
		case 'q':
			dumpqueues();
			return;
		case 'm':
			mntdump();
			return;
		case 'i':
			incontoggle();
			return;
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
void
kbdchar(int c)
{
	if(kbdq.repeat == 1){
		kbdq.c = c;
		kbdq.count = 0;
		kbdq.repeat = 2;
	}
	echo(c);
	*kbdq.in++ = c;
	if(kbdq.in == kbdq.buf+sizeof(kbdq.buf))
		kbdq.in = kbdq.buf;
	if(raw.ref || c=='\n' || c==0x04)
		wakeup(&kbdq.r);
}

void
kbdrepeat(int rep)
{
	if(rep)
		kbdq.repeat = 1;
	else
		kbdq.repeat = 0;
}

void
kbdclock(void)
{
	if(kbdq.repeat==2 && (++kbdq.count&1))
		kbdchar(kbdq.c);
}

int
consactive(void)
{
	return printq.printing;
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
	Qrcons,
	Qtime,
	Quser,
	Qklog,
	Qmsec,
	Qclock,
	Qrs232ctl = STREAMQID(1, Sctlqid),
	Qrs232 = STREAMQID(1, Sdataqid),
};

Dirtab consdir[]={
	"cons",		{Qcons},	0,		0600,
	"cputime",	{Qcputime},	6*NUMSIZE,	0600,
	"null",		{Qnull},	0,		0600,
	"pgrpid",	{Qpgrpid},	NUMSIZE,	0600,
	"pid",		{Qpid},		NUMSIZE,	0600,
	"ppid",		{Qppid},	NUMSIZE,	0600,
	"rcons",	{Qrcons},	0,		0600,
	"rs232",	{Qrs232},	0,		0600,
	"rs232ctl",	{Qrs232ctl},	0,		0600,
	"time",		{Qtime},	NUMSIZE,	0600,
	"user",		{Quser},	0,		0600,
	"klog",		{Qklog},	0,		0400,
	"msec",		{Qmsec},	NUMSIZE,	0400,
	"clock",	{Qclock},	2*NUMSIZE,	0400,
};

#define	NCONS	(sizeof consdir/sizeof(Dirtab))

static int
consgen(Chan *c, Dirtab *tab, int ntab, int i, Dir *dp)
{
	if(tab==0 || i>=ntab)
		return -1;
	tab += i;
	devdir(c, tab->qid, tab->name, tab->length, tab->perm, dp);
	return 1;
}

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
	return devwalk(c, name, consdir, NCONS, consgen);
}

void
consstat(Chan *c, char *dp)
{
	switch(c->qid.path){
	case Qrs232:
		streamstat(c, dp, "rs232");
		break;
	default:
		devstat(c, dp, consdir, NCONS, consgen);
		break;
	}
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
	case Qrs232:
	case Qrs232ctl:
		streamopen(c, &rs232info);
		break;
	}
	return devopen(c, omode, consdir, NCONS, consgen);
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
	if(c->stream)
		streamclose(c);
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
	switch(c->qid.path&~CHDIR){
	case Qdir:
		return devdirread(c, buf, n, consdir, NCONS, consgen);

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
					*lineq.in++ = ch;
					if(lineq.in == lineq.buf+sizeof(lineq.buf))
						lineq.in = lineq.buf;
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
		qunlock(&kbdq);
		return i;

	case Qrs232:
		return streamread(c, buf, n);

	case Qrs232ctl:
		if(c->offset)
			return 0;
		*(char *)buf = duartinputport();
		return 1;

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
		return readnum(c->offset, buf, n, boottime+TK2SEC(MACHP(0)->ticks), NUMSIZE);

	case Qmsec:
		return readnum(c->offset, buf, n, TK2MS(MACHP(0)->ticks), NUMSIZE);
	case Qclock:
		k = c->offset;
		if(k >= 2*NUMSIZE)
			return 0;
		if(k+n > 2*NUMSIZE)
			n = 2*NUMSIZE - k;
		readnum(0, tmp, NUMSIZE, MACHP(0)->ticks, NUMSIZE);
		readnum(0, tmp+NUMSIZE, NUMSIZE, HZ, NUMSIZE);
		memcpy(buf, tmp+k, n);
		return n;

	case Quser:
		return readstr(c->offset, buf, n, u->p->pgrp->user);

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

	switch(c->qid.path){
	case Qcons:
	case Qrcons:
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

	case Qrs232:
	case Qrs232ctl:
		n = streamwrite(c, va, n, 1);
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
			error(Eperm);
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
		error(Eperm);

	case Qnull:
		break;
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

/*
 *  rs232 stream routines
 *
 *  A kernel process, rs232kproc, stages blocks to be output and
 *  packages input bytes into stream blocks to send upstream.
 *  The process is awakened whenever the interrupt side is almost
 *  out of bytes to xmit or 1/16 second has elapsed since a byte
 *  was input.
 */
static int
rs232empty(void *a)
{
	Rs232 *r;

	r = a;
	return r->out.w == r->out.r;
}

static void
rs232output(Rs232 *r)
{
	int next;
	Queue *q;
	Block *bp;
	long l;

	qlock(&r->outlock);
	q = r->wq;

	/*
	 *  free old blocks
	 */
	for(next = r->out.f; next != r->out.r; next = NEXT(next)){
		freeb(r->out.bp[next]);
		r->out.bp[next] = 0;
	}
	r->out.f = next;

	/*
	 *  stage new blocks
	 *
	 *  if we run into a control block, wait till the queue
	 *  is empty before doing the control.
	 */
	for(next = NEXT(r->out.w); next != r->out.f; next = NEXT(next)){
		bp = getq(q);
		if(bp == 0)
			break;
		if(bp->type == M_CTL){
			while(!rs232empty(r))
				sleep(&r->r, rs232empty, r);
			l = strtoul((char *)(bp->rptr+1), 0, 0);
			switch(*bp->rptr){
			case 'B':
			case 'b':
				duartbaud(l);
				break;
			case 'D':
			case 'd':
				duartdtr(l);
				break;
			case 'K':
			case 'k':
				duartbreak(l);
				break;
			case 'W':
			case 'w':
				if(l>=0 && l<1000)
					r->delay = l;
				break;
			}
			freeb(bp);
			break;
		}
		r->out.bp[r->out.w] = bp;
		r->out.w = next;
	}

	/*
	 *  start output, the spl's sync with interrupt level
	 *  this wouldn't work on a multi-processor
	 */
	splhi();
	if(r->started == 0){
		r->started = 1;
		duartstartrs232o();
	}
	spllo();
	qunlock(&r->outlock);
}

static void
rs232input(Rs232 *r)
{
	Queue *q;
	int c;
	Block *bp;

	q = RD(r->wq);
	bp = 0;
	while((c = getc(&r->in)) >= 0){
		if(bp == 0){
			bp = allocb(64);
			bp->flags |= S_DELIM;
		}
		*bp->wptr++ = c;
		if(bp->wptr == bp->lim){
			if(QFULL(q->next))
				freeb(bp);
			else
				PUTNEXT(q, bp);
			bp = 0;
		}
	}
	if(bp){
		if(QFULL(q->next))
			freeb(bp);
		else
			PUTNEXT(q, bp);
	}
}

static int
rs232stuff(void *arg)
{
	Rs232 *r;

	r = arg;
	return (r->in.in != r->in.out) || (r->out.r != r->out.w)
		|| (r->out.f != r->out.r);
}

static void
rs232kproc(void *a)
{
	Rs232 *r;

	r = a;
	for(;;){
		qlock(r);
		if(r->wq != 0){
			rs232output(r);
			rs232input(r);
		}
		qunlock(r);
		sleep(&r->r, rs232stuff, r);
	}
}

static void
rs232open(Queue *q, Stream *c)
{
	Rs232 *r;

	r = &rs232;

	RD(q)->ptr = r;
	WR(q)->ptr = r;
	r->wq = WR(q);

	if(r->kstarted == 0){
		r->in.in = r->in.out = r->in.buf;
		kproc("rs232", rs232kproc, r);
		r->kstarted = 1;
	}
}

static void
rs232close(Queue *q)
{
	Rs232 *r;

	r = q->ptr;
	qlock(r);
	r->wq = 0;
	qunlock(r);
}

static void
rs232oput(Queue *q, Block *bp)
{
	if(bp->rptr >= bp->wptr)
		freeb(bp);
	else
		putq(q, bp);
	rs232output(q->ptr);
}

static void
rs232timer(Alarm *a)
{
	Rs232 *r;

	r = a->arg;
	cancel(a);
	r->a = 0;
	wakeup(&r->r);
}

/*
 *  called by input interrupt.  runs splhi
 */
void
rs232ichar(int c)
{
	Rs232 *r;

	r = &rs232;
	if(putc(&r->in, c) < 0)
		screenputc('^');

	/*
	 *  pass upstream within 1/16 second
	 */
	if(r->a==0){
		if(r->delay == 0)
			wakeup(&r->r);
		else
			r->a = alarm(r->delay, rs232timer, r);
	}
}

/*
 *  called by output interrupt.  runs splhi
 */
int
getrs232o(void)
{
	uchar c;
	Rs232 *r;
	Block *bp;

	r = &rs232;
	if(r->out.r == r->out.w){
		r->started = 0;
		return -1;
	}
	bp = r->out.bp[r->out.r];
	c = *bp->rptr++;
	if(bp->rptr >= bp->wptr){
		r->out.r = NEXT(r->out.r);
		if(r->out.r==r->out.w || NEXT(r->out.r)==r->out.w)
			wakeup(&r->r);
	}
	return c;
}
