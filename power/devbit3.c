#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"

#include	"io.h"

static struct
{
	QLock;
	QLock	buflock;
	int	open;
	char	buf[10*1024];
}bit3;

#define	BIT3ADDR	((Bit3msg**)(KZERO+0x7C))
#define	BIT3HOLD	((ulong*)(KZERO+0x78))
#define	BIT3INTR	((char*)(UNCACHED|0x17c12001))

enum
{
	RESET,
	READ,
	WRITE,
};

void
bit3send(Bit3msg *bp, ulong cmd, void *addr, ulong count)
{
	do; while(*BIT3ADDR);
	bp->cmd = cmd;
	bp->addr = (ulong)addr;
	bp->count = count;
	*BIT3ADDR = bp;
	wbflush();
	do; while(*BIT3HOLD);
	*BIT3INTR = 0x20;
}

void
bit3reset(void)
{
	*BIT3HOLD = 0;
	*BIT3ADDR = 0;
	qlock(&bit3);
	qunlock(&bit3);
}

void
bit3init(void)
{
}

Chan*
bit3attach(char *spec)
{
	return devattach('3', spec);
}

Chan*
bit3clone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
bit3walk(Chan *c, char *name)
{
	if(c->qid.path != CHDIR)
		return 0;
	if(strcmp(name, "bit3") == 0){
		c->qid.path = 1;
		return 1;
	}
	return 0;
}

void	 
bit3stat(Chan *c, char *dp)
{
	print("bit3stat\n");
	error(Egreg);
}

Chan*
bit3open(Chan *c, int omode)
{
	Bit3msg *bp;

	bp = &((User*)(u->p->upage->pa|KZERO))->kbit3;
	if(c->qid.path!=1 || omode!=ORDWR)
		error(Eperm);
	qlock(&bit3);
	if(bit3.open){
		qunlock(&bit3);
		error(Einuse);
	}
	bit3send(bp, RESET, 0, 0);
	bit3.open = 1;
	qunlock(&bit3);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
bit3create(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
bit3close(Chan *c)
{
	qlock(&bit3);
	bit3.open = 0;
	qunlock(&bit3);
}

/*
 * Read and write use physical addresses if they can, which they usually can.
 * Most I/O is from devmnt, which has local buffers.  Therefore just check
 * that buf is in KSEG0 and is at an even address.  The only killer is that
 * DMA counts from the bit3 device are mod 256, so devmnt must use oversize
 * buffers.
 */

long	 
bit3read(Chan *c, void *buf, long n)
{
	Bit3msg *bp;
	int docpy;

	switch(c->qid.path){
	case 1:
		if(n > sizeof bit3.buf)
			error(Egreg);
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			/*
			 *  use supplied buffer, no need to lock for reply
			 */
			bp = &((User*)(u->p->upage->pa|KZERO))->kbit3;
			bp->rcount = 0;
qlock(&bit3.buflock); /* BUG */
			qlock(&bit3);
			bit3send(bp, READ, buf, n);
			qunlock(&bit3);
			do
				n = bp->rcount;
			while(n == 0);
qunlock(&bit3.buflock); /* BUG */
		}else{
			/*
			 *  use bit3 buffer.  lock the buffer till the reply
			 */
			bp = &((User*)(u->p->upage->pa|KZERO))->ubit3;
			bp->rcount = 0;
			qlock(&bit3.buflock);
			qlock(&bit3);
			bit3send(bp, READ, bit3.buf, n);
			qunlock(&bit3);
			do
				n = bp->rcount;
			while(n == 0);
			memcpy(buf, bit3.buf, n);
			qunlock(&bit3.buflock);
		}
		return n;
	}
	error(Egreg);
	return 0;
}

long	 
bit3write(Chan *c, void *buf, long n)
{
	Bit3msg *bp;

	switch(c->qid.path){
	case 1:
		if(n > sizeof bit3.buf)
			error(Egreg);
		if((((ulong)buf)&(KSEGM|3)) == KSEG0){
			bp = &((User*)(u->p->upage->pa|KZERO))->kbit3;
			qlock(&bit3);
			bit3send(bp, WRITE, buf, n);
			qunlock(&bit3);
		}else{
			bp = &((User*)(u->p->upage->pa|KZERO))->ubit3;
			qlock(&bit3.buflock);

			qlock(&bit3);
			memcpy(bit3.buf, buf, n);
			bit3send(bp, WRITE, bit3.buf, n);
			do; while(*BIT3ADDR);
			qunlock(&bit3);

			qunlock(&bit3.buflock);
		}
		return n;
	}
	error(Egreg);
	return 0;
}

void	 
bit3remove(Chan *c)
{
	error(Eperm);
}

void	 
bit3wstat(Chan *c, char *dp)
{
	error(Eperm);
}
