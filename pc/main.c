#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"


extern ulong edata;

/*
 *  predeclared
 */
int	fdboot(void);
int	hdboot(void);
int	duartboot(void);
int	parse(char*);
int	getline(int);
int	getstr(char*, char*, int, char*, int);
int	menu(void);

char	file[2*NAMELEN];
char	server[NAMELEN];
char	sysname[NAMELEN];
char	user[NAMELEN] = "none";
char	linebuf[256];
Conf	conf;


typedef	struct Booter	Booter;
struct Booter
{
	char	*name;
	char	*srv;
	int	(*func)(void);
};
Booter	booter[] = {
	{ "fd",		0,	fdboot },
	{ "hd",		0,	hdboot },
	{ "2400",	0,	duartboot },
	{ "1200",	0,	duartboot },
};

int	bootdev;
char	*bootchars = "fh21";
int	usecache;

main(void)
{
	char	*path;			/* file path */
	char	element[2*NAMELEN];	/* next element in a file path */
	char	def[2*NAMELEN];

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

	for(;;){
		sprint(def, "%s!%s!/%s", booter[bootdev].name, booter[bootdev].srv,
			usecache ? "9.cache" : "9.com");
		if(getstr("server", element, sizeof element, def, 0)<0)
			continue;
		if(parse(element) < 0)
			continue;
		if(getstr("user", user, sizeof user, 0, 0)<0)
			continue;
		if(*user==0)
			continue;
		if((*booter[bootdev].func)() < 0)
			continue;
		print("success\n");
	}
}

/*
 *  parse the server line.  return 0 if OK, -1 if bullshit
 */
int
parse(char *line)
{
	char *def[3];
	char **d;
	char *s;
	int i;

	def[0] = booter[bootdev].name;
	def[1] = booter[bootdev].srv;
	def[2] = "/9.com";

	d = &def[2];
	s = line + strlen(line);
	while((*d = s) > line)
		if(*--s == '!'){
			if(d-- == def)
				return -1;
			*s = '\0';
		}

	for(i = 0; i < strlen(bootchars); i++){
		if(strcmp(def[0], booter[i].name)==0){
			strcpy(server, def[1]);
			strcpy(file, def[2]);
			bootdev = i;
			return 0;
		}
	}

	return -1;
}

/*
 *  read a line from the keyboard.
 */
int
getline(int quiet)
{
	int c, i=0;
	long start;

	for (start=m->ticks;;) {
	    	do{
			if(TK2SEC(m->ticks - start) > 60)
				return -2;
			c = getc(&kbdq);
		} while(c==-1);
		if(c == '\r')
			c = '\n'; /* turn carriage return into newline */
		if(c == '\177')
			c = '\010';	/* turn delete into backspace */
		if(!quiet){
			if(c == '\033'){
				menu();
				return -1;
			}
			if(c == '\025')
				screenputc('\n');	/* echo ^U as a newline */
			else
				screenputc(c);
		}
		if(c == '\010'){
			if(i > 0)
				i--; /* bs deletes last character */
			continue;
		}
		/* a newline ends a line */
		if (c == '\n')
			break;
		/* ^U wipes out the line */
		if (c =='\025')
			return -1;
		linebuf[i++] = c;
	}
	linebuf[i] = 0;
	return i;
}

/*
 *  prompt for a string from the keyboard.  <cr> returns the default.
 */
int
getstr(char *prompt, char *buf, int size, char *def, int quiet)
{
	int len;
	char *cp;

	for (;;) {
		if(def)
			print("%s[default==%s]: ", prompt, def);
		else
			print("%s: ", prompt);
		len = getline(quiet);
		switch(len){
		case -1:
			/* ^U typed */
			continue;
		case -2:
			/* timeout */
			return -1;
		default:
			break;
		}
		if(len >= size){
			print("line too long\n");
			continue;
		}
		break;
	}
	if(*linebuf==0 && def)
		strcpy(buf, def);
	else
		strcpy(buf, linebuf);
	return 0;
}

int
menu(void)
{
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

/*
 *  some dummy's so we can use kernel code
 */
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


/*
 *  boot from hard disk
 */
int
hdboot(void)
{
	print("hdboot unimplemented\n");
	return -1;
}

/*
 *  boot from the duart
 */
int
duartboot(void)
{
	print("duartboot unimplemented\n");
	return -1;
}

#include "dosfs.h"

/*
 *  boot from the floppy
 */
int
fdboot(void)
{
	Dosbpb b;
	extern int dosboot(Dosbpb*);

	print("booting from floppy 0\n");
	b.seek = floppyseek;
	b.read = floppyread;
	b.dev = 0;
	return dosboot(&b);
}
