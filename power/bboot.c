#include <u.h>
#include <libc.h>

#include <fcall.h>

#define DEFFILE "/mips/9"
#define DEFSYS "bitbootes"

Fcall	hdr;
char	*sys;
char	*scmd;
char	*bootfile;

char	sbuf[2*NAMELEN];
char	bbuf[5*NAMELEN];
char	buf[4*1024];

int fd;
int cfd;
int efd;


typedef
struct address {
	char *name;
	char *cmd;
} Address;

Address addr[] = {
	{ "bitbootes", "bitconnect" },
	{ "ross", "connect 020701005eff" },
	{ "bootes", "connect 080069020205" },
	{ "helix", "connect 080069020427" },
	{ "spindle", "connect 0800690202df" },
	{ "r70", "connect 08002b04265d" },
	{ 0 }
};

struct a_out_h {
	ulong	magic;			/* magic and sections */
	ulong	timestamp;		/* time and date */
	ulong	size;			/* (HEADR+textsize+datsize) */
	ulong	symsize;		/* nsyms */
	ulong	opt;			/* size of optional hdr and flags */
	ulong	magicversion;		/* magic and version */
	ulong	text;			/* sizes */
	ulong	data;
	ulong	bss;
	ulong	entryva;			/* va of entry */
	ulong	textva;			/* va of base of text */
	ulong	dataca;			/* va of base of data */
	ulong	bssva;			/* va of base of bss */
	ulong	gpregmask;		/* gp reg mask */
	ulong	dummy1;
	ulong	dummy1;
	ulong	dummy1;
	ulong	dummy1;
	ulong	gpvalue;		/* gp value ?? */
	ulong	mystery;		/* complete mystery */
} a_out;

/*
 *  predeclared
 */
char	*lookup(char *);
int	readseg(int, int, long, long, int);
int	readkernel(int);
int	outin(char *, char *, char *, int);
void	prerror(char *);
void	error(char *);
void	boot(int);

/*
 *  usage: 9b [-a] [server] [file]
 *
 *  default server is `bitbootes', default file is `/sys/src/9/mips/9'
 */
main(int argc, char *argv[])
{
	int i;

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	argv++;
	argc--;	
	while(argc > 0){
		if(argv[0][0] == '-'){
			argc--;
			argv++;
		} else
			break;
	}

	sys = DEFSYS;
	bootfile = DEFFILE;
	switch(argc){
	case 1:
		bootfile = argv[0];
		break;
	case 2:
		bootfile = argv[0];
		sys = argv[1];
		break;
	}

	boot(0);
	for(;;){
		if(fd > 0)
			close(fd);
		if(cfd > 0)
			close(cfd);
		if(efd > 0)
			close(efd);
		fd = cfd = efd = 0;
		boot(1);
	}
}

void
boot(int ask)
{
	int n;

	if(ask){
		outin("bootfile", bootfile, bbuf, sizeof(bbuf));
		bootfile = bbuf;
	}

	if(!ask)
		scmd = lookup(sys);
	else {
		outin("server", sys, sbuf, sizeof(sbuf));
		sys = sbuf;
		scmd = lookup(sys);
	}
	if(scmd == 0){
		fprint(2, "boot: %s unknown\n", sys);
		return;
	}

	print("Booting %s from server %s\n", bootfile, sys);

	/*
	 *  for the bit, we skip all the ether goo
	 */
	if(strcmp(scmd, "bitconnect") == 0){
		fd = open("#3/bit3", ORDWR);
		if(fd < 0){
			prerror("opening #3/bit3");
			return;
		}
		goto Mesg;
	}

	/*
	 *  grab a lance channel, make it recognize ether type 0x900,
	 *  and push the nonet ethernet multiplexor onto it.
	 */
	cfd = open("#l/1/ctl", 2);
	if(cfd < 0){
		prerror("opening #l/1/ctl");
		return;
	}
	if(write(cfd, "connect 0x900", sizeof("connect 0x900")-1)<0){
		prerror("connect 0x900");
		return;
	}
	if(write(cfd, "push noether", sizeof("push noether")-1)<0){
		prerror("push noether");
		return;
	}

	/*
	 *  grab a nonet channel and call up the ross file server
	 */
	fd = open("#n/1/data", 2);
	if(fd < 0) {
		prerror("opening #n/1/data");
		return;
	}
	cfd = open("#n/1/ctl", 2);
	if(cfd < 0){
		prerror("opening #n/1/ctl");
		return;
	}
	if(write(cfd, scmd, strlen(scmd))<0){
		prerror(scmd);
		return;
	}

    Mesg:
	print("nop...");
	hdr.type = Tnop;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n){
		prerror("write nop");
		return;
	}
	n = read(fd, buf, sizeof buf);
	if(n <= 0){
		prerror("read nop");
		return;
	}
	if(convM2S(buf, &hdr, n) == 0) {
		print("n = %d; buf = %.2x %.2x %.2x %.2x\n",
			n, buf[0], buf[1], buf[2], buf[3]);
		prerror("format nop");
		return;
	}
	if(hdr.type != Rnop){
		prerror("not Rnop");
		return;
	}

	print("session...");
	hdr.type = Tsession;
	hdr.lang = 'v';
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n){
		prerror("write session");
		return;
	}
	n = read(fd, buf, sizeof buf);
	if(n <= 0){
		prerror("read session");
		return;
	}
	if(convM2S(buf, &hdr, n) == 0){
		prerror("format session");
		return;
	}
	if(hdr.type != Rsession){
		prerror("not Rsession");
		return;
	}
	if(hdr.err){
		print("error %d;", hdr.err);
		prerror("remote error");
		return;
	}

	print("mount...");
	if(bind("/", "/", MREPL) < 0){
		prerror("bind");
		return;
	}
	if(mount(fd, "/", MAFTER|MCREATE, "") < 0){
		prerror("mount");
		return;
	}
	close(fd);

	print("open file...");
	while((fd = open(bootfile, OREAD)) < 0){
		outin("bootfile", DEFFILE, bbuf, sizeof(bbuf));
		bootfile = bbuf;
	}
	readkernel(fd);
	prerror("couldn't read kernel");
}

/*
 *  print error
 */
void
prerror(char *s)
{
	char buf[64];

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
}

/*
 *  print error and exit
 */
void
error(char *s)
{
	char buf[64];

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}

/*
 *  lookup the address for a system
 */
char *
lookup(char *arg)
{
	Address *a;

	if(strcmp(arg, "?")==0 || strcmp(arg, "help")==0){
		for(a = addr; a->name; a++)
			print("%s\n", a->name);
		return 0;
	}
	for(a = addr; a->name; a++){
		if(strcmp(a->name, arg) == 0)
			return a->cmd;
	}
	return 0;
}

/*
 *  read a segment into memory
 */
int
readseg(int in, int out, long inoff, long outoff, int len)
{
	long	n, i;
	ulong sum = 0;

	if(seek(in, inoff, 0) < 0){
		prerror("seeking bootfile");
		return -1;
	}
	if(seek(out, outoff, 0) != outoff){
		prerror("seeking #b/mem");
		return -1;
	}
	for(; len > 0; len -= n){
		if((n = read(in, buf, sizeof buf)) <= 0){
			prerror("reading bootfile");
			return -1;
		}
		for(i = 0; i < n; i++)
			sum += buf[i];
		if(sum & 0xf0000000)
			sum = (sum & 0xfffffff) + ((sum & 0xf0000000) >> 28);
		if(write(out, buf, n) != n){
			prerror("writing #b/mem");
			return -1;
		}
	}
	print("[%ux]", sum);
	return 0;
}

/*
 *  read the kernel into memory and jump to it
 */
int
readkernel(int fd)
{
	int n;
	int bfd;

	bfd = open("#b/mem", OWRITE);
	if(bfd < 0){
		prerror("can't open #b/mem");
		return;
	}

	n = read(fd, &a_out, sizeof(a_out));
	if(n <= 0){
		prerror("can't read boot file");
		close(bfd);
		return;
	}

	print("\n%d", a_out.text);
	if(readseg(fd, bfd, 20*4, a_out.textva, a_out.text)<0){
		prerror("can't read boot file");
		close(bfd);
		return;
	}
	print("+%d", a_out.data);
	if(readseg(fd, bfd, 20*4 + a_out.text, a_out.textva + a_out.text, a_out.data)<0){
		prerror("can't read boot file");
		close(bfd);
		return;
	}
	print("+%d\n", a_out.bss);

	close(bfd);
	bfd = open("#b/boot", OWRITE);
	if(bfd < 0){
		prerror("can't open #b/boot");
		return;
	}
	
	print("entry: %ux\n", a_out.entryva);
	sleep(1000);
	if(write(bfd, &a_out.entryva, sizeof a_out.entryva) != sizeof a_out.entryva){
		prerror("can't start kernel");
		close(bfd);
	}

	return;
}

/*
 *  prompt and get input
 */
int
outin(char *prompt, char *def, char *buf, int len)
{
	int n;

	do{
		print("%s[%s]: ", prompt, def);
		n = read(0, buf, len);
	}while(n==0);
	if(n < 0)
		error("can't read #c/cons; please reboot");
	if(n == 1)
		strcpy(buf, def);
	else
		buf[n-1] = 0;
	return n;
}
