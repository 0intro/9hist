#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"

void
initq(IOQ *q)
{
	q->in = q->out = q->buf;
	q->puts = puts;
	q->putc = putc;
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

void
puts(IOQ *q, char *s, int n)
{
	int m;

	while(n){
		if(q->out > q->in)
			m = q->out - q->in - 1;
		else
			m = &q->buf[NQ] - q->in;
		if(m > n)
			m = n;
		memcpy(q->in, s, m);
		n -= m;
		q->in += m;
		if(q->in >= &q->buf[NQ])
			q->in = q->buf;
	}
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
