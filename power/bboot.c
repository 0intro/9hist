#include <u.h>
#include <libc.h>

#include <fcall.h>

void	error(char *);
char	buf[4*1024];

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

#define DEFFILE "/sys/src/9/mips/9"

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
	long	n;

	if(seek(in, inoff, 0) < 0){
		error("seeking bootfile");
		return -1;
	}
	if(seek(out, outoff, 0) != outoff){
		error("seeking #b/mem");
		return -1;
	}
	for(; len > 0; len -= n){
		print(".");
		if((n = read(in, buf, sizeof buf)) <= 0){
			error("reading bootfile");
			return -1;
		}
		if(write(out, buf, n) != n){
			error("writing #b/mem");
			return -1;
		}
	}
	return 0;
}

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
 *  read the kernel into memory and jump to it
 */
int
readkernel(int fd)
{
	int n;
	int bfd;

	bfd = open("#b/mem", OWRITE);
	if(bfd < 0)
		error("can't open #b/mem");

	n = read(fd, &a_out, sizeof(a_out));
	if(n <= 0)
		error("can't read boot file");

	print("\n%d", a_out.text);
	if(readseg(fd, bfd, 20*4, a_out.textva, a_out.text)<0)
		error("can't read boot file");
	print("+%d", a_out.data);
	if(readseg(fd, bfd, 20*4 + a_out.text, a_out.textva + a_out.text, a_out.data)<0)
		error("can't read boot file");
	print("+%d\n", a_out.bss);

	close(bfd);
	bfd = open("#b/boot", OWRITE);
	if(bfd < 0)
		error("can't open #b/boot");
	
	print("entry: %ux\n", a_out.entryva);
	sleep(1000);
	if(write(bfd, &a_out.entryva, sizeof a_out.entryva) != sizeof a_out.entryva)
		error("can't start kernel");

	return 0;
}

Fcall	hdr;
char	srv[100];

main(int argc, char *argv[])
{
	int cfd, fd, n, fu, f;
	char buf[NAMELEN];
	char bootfile[256];
	char *scmd;

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	/*
	 *  get server
	 */
	do{
		do{
			print("server[%s]: ", addr[0].name);
			n = read(0, srv, sizeof srv);
		}while(n==0);
		if(n < 0)
			error("can't read #c/cons; please reboot");
		if(n == 1)
			strcpy(srv, addr[0].name);
		else
			srv[n-1] = 0;
		scmd = lookup(srv);
	}while(scmd == 0);

	/*
	 *  get file.  if the user typed cr to the server question, skip
	 *  the file question and just use the default.
	 */
	if(n != 1){
		do{
			print("bootfile[%s]: ", DEFFILE);
			n = read(0, buf, sizeof buf);
		}while(n==0);
		if(n < 0)
			error("can't read #c/cons; please reboot");
		if(n == 1)
			strcpy(buf, DEFFILE);
		else
			buf[n-1] = 0;
		strcpy(bootfile, buf);
	}else
		strcpy(bootfile, DEFFILE);

	if(strcmp(scmd, "bitconnect") == 0){
		fd = open("#3/bit3", ORDWR);
		if(fd < 0)
			error("opening #3/bit3");
		goto Mesg;
	}

	/*
	 *  grab a lance channel, make it recognize ether type 0x900,
	 *  and push the nonet ethernet multiplexor onto it.
	 */
	cfd = open("#l/1/ctl", 2);
	if(cfd < 0)
		error("opening #l/1/ctl");
	if(write(cfd, "connect 0x900", sizeof("connect 0x900")-1)<0)
		error("connect 0x900");
	if(write(cfd, "push noether", sizeof("push noether")-1)<0)
		error("push noether");

	/*
	 *  grab a nonet channel and call up the ross file server
	 */
	fd = open("#n/1/data", 2);
	if(fd < 0)
		error("opening #n/1/data");
	cfd = open("#n/1/ctl", 2);
	if(cfd < 0)
		error("opening #n/1/ctl");
	if(write(cfd, scmd, strlen(scmd))<0)
		error(scmd);

    Mesg:
	print("nop...");
	hdr.type = Tnop;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write nop");
	n = read(fd, buf, sizeof buf);
	if(n <= 0)
		error("read nop");
	if(convM2S(buf, &hdr, n) == 0) {
		print("n = %d; buf = %.2x %.2x %.2x %.2x\n",
			n, buf[0], buf[1], buf[2], buf[3]);
		error("format nop");
	}
	if(hdr.type != Rnop)
		error("not Rnop");

	print("session...");
	hdr.type = Tsession;
	hdr.lang = 'v';
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write session");
	n = read(fd, buf, sizeof buf);
	if(n <= 0)
		error("read session");
	if(convM2S(buf, &hdr, n) == 0)
		error("format session");
	if(hdr.type != Rsession)
		error("not Rsession");
	if(hdr.err){
		print("error %d;", hdr.err);
		error("remote error");
	}

	print("mount...");
	if(bind("/", "/", MREPL) < 0)
		error("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "") < 0)
		error("mount");
	close(fd);

	print("open file...");
	fd = open(bootfile, OREAD);
	if(fd < 0)
		error("opening bootfile");
	readkernel(fd);
	error("couldn't read kernel");
}

void
error(char *s)
{
	char buf[64];

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
