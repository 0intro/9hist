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
	int	open;
	char	buf[10*1024];
}bit;

#define	BITADDR	((Bitmsg**)(KZERO+0x7C))
#define	BITHOLD	((ulong*)(KZERO+0x78))
#define	BITINTR	((char*)(UNCACHED|0x17c12001))

enum
{
	RESET,
	READ,
	WRITE,
};

long	hold, wait, hang;

void
bitsend(Bitmsg *bp, ulong cmd, void *addr, ulong count)
{
	do wait++; while(*BITADDR);
	bp->cmd = cmd;
	bp->addr = (ulong)addr;
	bp->count = count;
/* print("%d %lux %d ", cmd, addr, count); /**/
	*BITADDR = bp;
	wbflush();
	do hold++; while(*BITHOLD);
	*BITINTR = 0x20;
/* print("done\n");  /**/
}

void
bitreset(void)
{
	*BITHOLD = 0;
	*BITADDR = 0;
	qlock(&bit);
	qunlock(&bit);
}

void
bitinit(void)
{
}

Chan*
bitattach(char *spec)
{
	return devattach('b', spec);
}

Chan*
bitclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int	 
bitwalk(Chan *c, char *name)
{
	if(c->qid != CHDIR)
		return 0;
	if(strcmp(name, "bit") == 0){
		c->qid = 1;
		return 1;
	}
	return 0;
}

void	 
bitstat(Chan *c, char *dp)
{
	print("bitstat\n");
	error(0, Egreg);
}

Chan*
bitopen(Chan *c, int omode)
{
	Bitmsg *bp;

	bp = &((User*)(u->p->upage->pa|KZERO))->bit;
	if(c->qid!=1 || omode!=ORDWR)
		error(0, Eperm);
	qlock(&bit);
	if(bit.open){
		qunlock(&bit);
		error(0, Einuse);
	}
	bitsend(bp, RESET, 0, 0);
	bit.open = 1;
	qunlock(&bit);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
bitcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(0, Eperm);
}

void	 
bitclose(Chan *c)
{
	qlock(&bit);
	bit.open = 0;
	qunlock(&bit);
}

/*
 * Read and write use physical addresses if they can, which they usually can.
 * Most I/O is from devmnt, which has local buffers.  Therefore just check
 * that buf is in KSEG0 and is at an even address.  The only killer is that
 * DMA counts from the bit device are mod 256, so devmnt must use oversize
 * buffers.
 */

long	 
bitread(Chan *c, void *buf, long n)
{
	Bitmsg *bp;
	int docpy;

	bp = &((User*)(u->p->upage->pa|KZERO))->bit;
	bp->rcount = 0;
	switch(c->qid){
	case 1:
		if(n > sizeof bit.buf)
			error(0, Egreg);
		docpy = 0;
		qlock(&bit);
		if((((ulong)buf)&(KSEGM|3)) == KSEG0)
			bitsend(bp, READ, buf, n);
		else{
			bitsend(bp, READ, bit.buf, n);
			docpy = 1;
		}
		qunlock(&bit);
		do{
			n = bp->rcount;
			hang++;
		}while(n == 0);
		if(docpy)
			memcpy(buf, bit.buf, n);
if(0 && n > 512){
	int i;
	char *cp=buf;
	for(i=9; i<n; i++)
		if(cp[i] != cp[i-1]){
			print("r %d %x %x\n", i, cp[i-1], cp[i]);
			break;
		}
}
		return n;
	}
	error(0, Egreg);
	return 0;
}

long	 
bitwrite(Chan *c, void *buf, long n)
{
	Bitmsg *bp;

	bp = &((User*)(u->p->upage->pa|KZERO))->bit;
	switch(c->qid){
	case 1:
		if(n > sizeof bit.buf)
			error(0, Egreg);
if(0 && n > 512){
	int i;
	char *cp=buf;
	for(i=15; i<n; i++)
		if(cp[i] != cp[i-1]){
			print("w %d %x %x\n", i, cp[i-1], cp[i]);
			break;
		}
}
		qlock(&bit);
		if((((ulong)buf)&(KSEGM|3)) == KSEG0)
			bitsend(bp, WRITE, buf, n);
		else{
			memcpy(bit.buf, buf, n);
			bitsend(bp, WRITE, bit.buf, n);
		}
		qunlock(&bit);
		return n;
	}
	error(0, Egreg);
	return 0;
}

void	 
bitremove(Chan *c)
{
	error(0, Eperm);
}

void	 
bitwstat(Chan *c, char *dp)
{
	error(0, Eperm);
}

void
bituserstr(Error *e, char *buf)
{
	consuserstr(e, buf);
}

void	 
biterrstr(Error *e, char *buf)
{
	rooterrstr(e, buf);
}
