#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


char sysname[NAMELEN];
Conf	conf;
char	user[NAMELEN] = "bootes";

extern ulong edata;

main(void)
{
	int i;

	machinit();
	confinit();
	screeninit();
	print("%d pages in bank0, %d pages in bank1\n", conf.npage0, conf.npage1);
	print("edata == %lux, end == %lux\n", &edata, &end);
	trapinit();
	mmuinit();
	clockinit();
	alarminit();
	kbdinit();
	clockinit();
	floppyinit();
	spllo();

	for(i=0; i<100; i++){
		int c;

		c = getc(&kbdq);
		if(c!=-1)
			screenputc(c);
		idle();
		floppyseek(0, 0);
		floppyseek(0, 18*512*20);
	}
	for(;;);
}

void
machinit(void)
{
	int n;

	n = m->machno;
	memset(m, 0, sizeof(Mach));
	m->machno = n;
	m->mmask = 1<<m->machno;
	active.machs = 1;
}

void
delay(int l)
{
	int i;

	while(--l){
		for(i=0; i < 404; i++)
			;
	}
}

void
confinit(void)
{
	long x, i, j, *l;
	int mul;

	/*
	 *  size memory
	 */
	x = 0x12345678;
	for(i=1; i<16; i++){
		/*
		 *  write the word
		 */
		l = (long*)(KZERO|(i*1024L*1024L));
		*l = x;
		/*
		 *  take care of wraps
		 */
		for(j = 0; j < i; j++){
			l = (long*)(KZERO|(j*1024L*1024L));
			*l = 0;
		}
		/*
		 *  check
		 */
		l = (long*)(KZERO|(i*1024L*1024L));
		if(*l != x)
			break;
		x += 0x3141526;
	}

	/*
	 *  the first 640k is the standard useful memory
	 */
	conf.npage0 = 640/4;
	conf.base0 = 0;

	/*
	 *  the last 128k belongs to the roms
	 */
	conf.npage1 = (i)*1024/4;
	conf.base1 = 1024/4;

	conf.npage = conf.npage0 + conf.npage1;
	conf.maxialloc = (640*1024-256*1024-BY2PG);

	mul = 1;
	conf.nproc = 20 + 50*mul;
	conf.npgrp = conf.nproc/2;
	conf.nseg = conf.nproc*3;
	conf.npagetab = (conf.nseg*14)/10;
	conf.nswap = conf.nproc*80;
	conf.nimage = 50;
	conf.nalarm = 1000;
	conf.nchan = 4*conf.nproc;
	conf.nenv = 4*conf.nproc;
	conf.nenvchar = 8000*mul;
	conf.npgenv = 200*mul;
	conf.nmtab = 50*mul;
	conf.nmount = 80*mul;
	conf.nmntdev = 15*mul;
	conf.nmntbuf = conf.nmntdev+3;
	conf.nmnthdr = 2*conf.nmntdev;
	conf.nsrv = 16*mul;			/* was 32 */
	conf.nbitmap = 512*mul;
	conf.nbitbyte = conf.nbitmap*1024*screenbits();
	conf.nfont = 10*mul;
	conf.nnoifc = 1;
	conf.nnoconv = 32;
	conf.nurp = 32;
	conf.nasync = 1;
	conf.nstream = (conf.nproc*3)/2;
	conf.nqueue = 5 * conf.nstream;
	conf.nblock = 24 * conf.nstream;
	conf.npipe = conf.nstream/2;
	conf.copymode = 0;			/* copy on write */
	conf.ipif = 8;
	conf.ip = 64;
	conf.arp = 32;
	conf.frag = 32;
	conf.cntrlp = 0;
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
	screenputs(buf, n);
	return n;
}

void
panic(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	screenputs("panic: ", 7);
	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	screenputs(buf, n);
	screenputs("\n", 1);
	spllo();
	for(;;)
		idle();
}

int
kbdputc(IOQ* q, int c)
{
	if(c==0x10)
		panic("^p");
	putc(q, c);
}

struct Palloc palloc;

void*
ialloc(ulong n, int align)
{
	ulong p;

	if(palloc.active && n!=0)
		print("ialloc bad\n");
	if(palloc.addr == 0)
		palloc.addr = ((ulong)&end)&~KZERO;
	if(align)
		palloc.addr = PGROUND(palloc.addr);

	memset((void*)(palloc.addr|KZERO), 0, n);
	p = palloc.addr;
	palloc.addr += n;
	if(align)
		palloc.addr = PGROUND(palloc.addr);

	if(palloc.addr >= conf.maxialloc)
		panic("keep bill joy away");

	return (void*)(p|KZERO);
}

void
sched(void)
{ }

void
ready(Proc*p)
{ }

int
postnote(Proc*p, int x, char* y, int z)
{
	panic("postnote");
}
