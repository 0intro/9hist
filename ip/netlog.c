#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../ip/ip.h"

int logmask;				/* mask of things to debug */
Ipaddr iponly;				/* ip address to print debugging for */

enum {
	Nlog		= 4*1024,
};

/*
 *  action log
 */
typedef struct Log {
	Lock;
	int	opens;
	char*	buf;
	char	*end;
	char	*rptr;
	int	len;

	QLock;
	Rendez;
} Log;
static Log alog;

typedef struct Logflag {
	char*	name;
	int	mask;
} Logflag;
static Logflag flags[] =
{
	{ "ppp",	Logppp, },
	{ "ip",		Logip, },
	{ "fs",		Logfs, },
	{ "tcp",	Logtcp, },
	{ "il",		Logil, },
	{ "icmp",	Logicmp, },
	{ "udp",	Logudp, },
	{ "compress",	Logcompress, },
	{ "ilmsg",	Logil|Logilmsg, },
	{ "gre",	Loggre, },
	{ "tcpmsg",	Logtcp|Logtcpmsg, },
	{ nil,		0, },
};

static char Ebadnetctl[] = "unknown netlog ctl message";

void
netlogopen(void)
{
	lock(&alog);
	if(waserror()){
		unlock(&alog);
		nexterror();
	}
	if(alog.opens == 0){
		if(alog.buf == nil)
			alog.buf = malloc(Nlog);
		alog.rptr = alog.buf;
		alog.end = alog.buf + Nlog;
	}
	alog.opens++;
	unlock(&alog);
	poperror();
}

void
netlogclose(void)
{
	lock(&alog);
	if(waserror()){
		unlock(&alog);
		nexterror();
	}
	alog.opens--;
	if(alog.opens == 0){
		free(alog.buf);
		alog.buf = nil;
	}
	unlock(&alog);
	poperror();
}

static int
netlogready(void*)
{
	return alog.len;
}

long
netlogread(void* a, ulong, long n)
{
	int i, d;
	char *p, *rptr;

	qlock(&alog);
	if(waserror()){
		qunlock(&alog);
		nexterror();
	}

	for(;;){
		lock(&alog);
		if(alog.len){
			if(n > alog.len)
				n = alog.len;
			d = 0;
			rptr = alog.rptr;
			alog.rptr += n;
			if(alog.rptr >= alog.end){
				d = alog.rptr - alog.end;
				alog.rptr = alog.buf + d;
			}
			alog.len -= n;
			unlock(&alog);

			i = n;
			p = a;
			if(d){
				memmove(p, rptr, d);
				i -= d;
				p += d;
				rptr = alog.buf;
			}
			memmove(p, rptr, i);
			break;
		}
		else
			unlock(&alog);

		sleep(&alog, netlogready, 0);
	}

	qunlock(&alog);
	poperror();

	return n;
}

char*
netlogctl(char* s, int len)
{
	int i, n, set;
	Logflag *f;
	char *fields[10], *p, buf[256];
	uchar addr[Ipaddrlen];

	if(len == 0)
		return Ebadnetctl;

	if(len >= sizeof(buf))
		len = sizeof(buf)-1;
	strncpy(buf, s, len);
	buf[len] = 0;
	if(len > 0 && buf[len-1] == '\n')
		buf[len-1] = 0;

	n = parsefields(buf, fields, 10, " ");
	if(n < 2)
		return Ebadnetctl;

	if(strcmp("set", fields[0]) == 0)
		set = 1;
	else if(strcmp("clear", fields[0]) == 0)
		set = 0;
	else if(strcmp("only", fields[0]) == 0){
		iponly = parseip(addr, fields[1]);
		return nil;
	} else
		return Ebadnetctl;

	p = strchr(fields[n-1], '\n');
	if(p)
		*p = 0;

	for(i = 1; i < n; i++){
		for(f = flags; f->name; f++)
			if(strcmp(f->name, fields[i]) == 0)
				break;
		if(f->name == nil)
			continue;
		if(set)
			logmask |= f->mask;
		else
			logmask &= ~f->mask;
	}

	return nil;
}

void
netlog(int mask, char *fmt, ...)
{
	char buf[128], *t, *f;
	int i, n;
	va_list arg;

	if(alog.opens == 0 || !(logmask & mask))
		return;

	va_start(arg, fmt);
	n = doprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	if(alog.opens == 0)
		return;

	lock(&alog);
	i = alog.len + n - Nlog;
	if(i > 0){
		alog.len -= i;
		alog.rptr += i;
		if(alog.rptr >= alog.end)
			alog.rptr = alog.buf + (alog.rptr - alog.end);
	}
	t = alog.rptr + alog.len;
	f = buf;
	alog.len += n;
	while(n-- > 0){
		if(t >= alog.end)
			t = alog.buf + (t - alog.end);
		*t++ = *f++;
	}
	unlock(&alog);

	wakeup(&alog);
}
