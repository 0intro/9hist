#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

static Queue *freed;

/*
 *  Interrupt handlers use freeb() to release blocks.  They are
 *  garbage collected by the kproc running bgc().
 */
static void
bgc(void *arg)
{
	Block *b, *nb;

	USED(arg);
	for(;;){
		tsleep(&freed->r, return0, 0, 500);
		if(freed->first == 0)
			continue;

		x = slphi();
		lock(&freed);
		b = freed->first;
		freed->first = freed->last = 0;;
		unlock(&freed);
		spllo();

		for(; b; b = nb){
			nb = b->next;
			free(b);
		}
	}
}

void
freeb(Block *b)
{
	lock(&freed);
	b->next = freed->first;
	freed->first = b;
	unlock(&freed);
}

void
blockinit(void)
{
	/* start garbage collector */
	kproc("buffer", bgc, 0);
}

/*
 *  allocate queues and blocks
 */
Queue*
allocq(int limit)
{
	Queue *q;

	q = smalloc(sizeof(Queue));
	q->limit = limit;
}

Block*
allocb(int size)
{
	Block *b;

	b = alloc(sizeof(Block) + size);
	if(b == 0)
		exhausted("blocks");

	b->base = (uchar*)(b+1);
	b->rp = b->wp = b->base;
	b->lim = b->base + size;

	return b;
}

/*
 *  copy out of a queue, returns # bytes copied
 */
int
consume(Queue *q, uchar *p, int len, int drop)
{
	Block *b;
	int n;

	lock(q);
	b = q->first;
	if(b == 0){
		q->state |= Qcsleep;
		unlock(q);
		return -1;
	}
	n = BLEN(b);
	if(n < len){
		memmove(p, b->rp, n);
	} else {
		memmove(p, b->rp, len);
}
