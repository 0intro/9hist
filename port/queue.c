#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

void
initq(IOQ *q)
{
	lock(q);
	unlock(q);
	q->in = q->out = q->buf;
	q->puts = puts;
	q->putc = putc;
}

int
putc(IOQ *q, int c)
{
	uchar *next;

	if(q->in ==  &q->buf[NQ-1])
		next = q->buf;
	else
		next = q->in+1;
	if(next == q->out)
		return -1;
	*q->in = c;
	q->in = next;
	return 0;
}

int
getc(IOQ *q)
{
	int c;

	if(q->in == q->out)
		return -1;
	c = *q->out;
	if(q->out == &q->buf[NQ-1])
		q->out = q->buf;
	else
		q->out++;
	return c;
}

void
puts(IOQ *q, void *buf, int n)
{
	uchar *next;
	uchar *p = buf;

	for(; n; n--){
		if(q->in == &q->buf[NQ-1])
			next = q->buf;
		else
			next = q->in + 1;
		if(next == q->out)
			break;
		*q->in = *p++;
		q->in = next;
	}
}

int
gets(IOQ *q, void *buf, int n)
{
	uchar *p = buf;

	for(; n && q->out != q->in; n--){
		*p++ = *q->out;
		if(q->out == &q->buf[NQ-1])
			q->out = q->buf;
		else
			q->out++;
	}
	return p - (uchar*)buf;
}

int
cangetc(void *arg)
{
	IOQ *q;
	int n;

	q = (IOQ *)arg;
	n = q->in - q->out;
	if (n < 0)
		n += sizeof(q->buf);
	return n;
}

int
canputc(void *arg)
{
	IOQ *q;
	int n;

	q = (IOQ *)arg;
	n = q->out - q->in - 1;
	if (n < 0)
		n += sizeof(q->buf);
	return n;
}
