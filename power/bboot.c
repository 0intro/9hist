#include <u.h>
#include <libc.h>

#include <fcall.h>

#define DEFFILE "/mips/9power"
#define DEFSYS "bit!bootes"

Fcall	hdr;
char	*scmd;
char	bootfile[5*NAMELEN];
char	conffile[5*NAMELEN];
char	sys[NAMELEN];

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
	{ "ross", "connect 020701005eff" },
	{ "bootes", "connect 0800690203f3" },
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
int	dkdial(char *);
int	nonetdial(char *);
int	bitdial(char *);
int	readseg(int, int, long, long, int);
int	readkernel(int);
int	readconf(int);
int	outin(char *, char *, int);
void	prerror(char *);
void	error(char *);
void	boot(int, char *);

/*
 *  usage: 9b [-a] [server] [file]
 *
 *  default server is `bitbootes', default file is `/sys/src/9/mips/9'
 */
main(int argc, char *argv[])
{
	int i;
	char *sysname;

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	sysname = argv[0];

	argv++;
	argc--;	
	while(argc > 0){
		if(argv[0][0] == '-'){
			argc--;
			argv++;
		} else
			break;
	}

	strcpy(sys, DEFSYS);
	strcpy(bootfile, DEFFILE);
	switch(argc){
	case 1:
		strcpy(bootfile, argv[0]);
		break;
	case 2:
		strcpy(bootfile, argv[0]);
		strcpy(sys, argv[1]);
		break;
	}

	boot(0, sysname);
	for(;;){
		if(fd > 0)
			close(fd);
		if(cfd > 0)
			close(cfd);
		fd = cfd = 0;
		boot(1, sysname);
	}
}

int
bitdial(char *arg)
{
	return open("#3/bit3", ORDWR);
}

int
nonetdial(char *arg)
{
	int efd, cfd, fd;
	Address *a;
	static int mounted;

	for(a = addr; a->name; a++){
		if(strcmp(a->name, arg) == 0)
			break;
	}
	if(a->name == 0){
		print("can't convert nonet address to ether address\n");
		return -1;
	}

	if(!mounted){
		/*
		 *  grab a lance channel, make it recognize ether type 0x900,
		 *  and push the nonet ethernet multiplexor onto it.
		 */
		efd = open("#l/1/ctl", 2);
		if(efd < 0){
			prerror("opening #l/1/ctl");
			return -1;
		}
		if(write(efd, "connect 0x900", sizeof("connect 0x900")-1)<0){
			close(efd);
			prerror("connect 0x900");
			return -1;
		}
		if(write(efd, "push noether", sizeof("push noether")-1)<0){
			close(efd);
			prerror("push noether");
			return -1;
		}
		if(write(efd, "config nonet", sizeof("config nonet")-1)<0){
			close(efd);
			prerror("config nonet");
			return -1;
		}
		mounted = 1;
	}

	/*
	 *  grab a nonet channel and call up the file server
	 */
	fd = open("#nnonet/2/data", 2);
	if(fd < 0) {
		prerror("opening #nnonet/2/data");
		return -1;
	}
	cfd = open("#nnonet/2/ctl", 2);
	if(cfd < 0){
		close(fd);
		fd = -1;
		prerror("opening #nnonet/2/ctl");
		return -1;
	}
	if(write(cfd, a->cmd, strlen(a->cmd))<0){
		close(cfd);
		close(fd);
		cfd = fd = -1;
		prerror(a->cmd);
		return -1;
	}
	return fd;
}

int
dkdial(char *arg)
{
	int fd;
	char cmd[64];
	static int mounted;

	if(!mounted){
		/*
		 *  grab the hsvme and configure it for a datakit
		 */
		efd = open("#h/ctl", 2);
		if(efd < 0){
			prerror("opening #h/ctl");
			return -1;
		}
		if(write(efd, "push dkmux", sizeof("push dkmux")-1)<0){
			close(efd);
			prerror("push dkmux");
			return -1;
		}
		if(write(efd, "config 4 256 restart dk", sizeof("config 4 256 restart dk")-1)<0){
			close(efd);
			prerror("config 4 256 restart dk");
			return -1;
		}
		mounted = 1;
		sleep(2000);		/* wait for things to settle down */
	}

	/*
	 *  grab a datakit channel and call up the file server
	 */
	fd = open("#k/dk/5/data", 2);
	if(fd < 0) {
		prerror("opening #k/dk/5/data");
		return -1;
	}
	cfd = open("#k/dk/5/ctl", 2);
	if(cfd < 0){
		close(fd);
		fd = -1;
		prerror("opening #k/dk/5/ctl");
		return -1;
	}
	sprint(cmd, "connect %s", arg);
	if(write(cfd, cmd, strlen(cmd))<0){
		close(cfd);
		close(fd);
		cfd = fd = -1;
		prerror(cmd);
		return -1;
	}
	return fd;
}

void
boot(int ask, char *addr)
{
	int n, tries;
	char *srvname;

	if(ask){
		outin("bootfile", bootfile, sizeof(bootfile));
		outin("server", sys, sizeof(sys));
	}

	for(tries = 0; tries < 5; tries++){
		fd = -1;
		if(strncmp(sys, "bit!", 4) == 0)
			fd = bitdial(srvname = &sys[4]);
		else if(strncmp(sys, "dk!", 3) == 0)
			fd = dkdial(srvname = &sys[3]);
		else if(strncmp(sys, "nonet!", 6) == 0)
			fd = nonetdial(srvname = &sys[6]);
		else
			fd = nonetdial(srvname = sys);
		if(fd >= 0)
			break;
		print("can't connect, retrying...\n");
		sleep(1000);
	}
	if(fd < 0){
		print("can't connect\n");
		return;
	}

	print("nop...");
	hdr.type = Tnop;
	hdr.tag = NOTAG;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n){
		print("n = %d\n", n);
		prerror("write nop");
		return;
	}
  reread:
	n = read(fd, buf, sizeof buf);
	if(n <= 0){
		prerror("read nop");
		return;
	}
	if(n == 2)
		goto reread;
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
	if(hdr.tag != NOTAG){
		prerror("tag not NOTAG");
		return;
	}

	print("session...");
	hdr.type = Tsession;
	hdr.tag = NOTAG;
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
	if(hdr.tag != NOTAG){
		prerror("tag not NOTAG");
		return;
	}
	if(hdr.type == Rerror){
		fprint(2, "boot: error %s\n", hdr.ename);
		return;
	}
	if(hdr.type != Rsession){
		prerror("not Rsession");
		return;
	}

	print("mount...");
	if(bind("/", "/", MREPL) < 0){
		prerror("bind");
		return;
	}
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0){
		prerror("mount");
		return;
	}
	close(fd);

	sprint(conffile, "/mips/conf/%s", addr);
	print("%s...", conffile);
	while((fd = open(conffile, OREAD)) < 0){
		outin("conffile", conffile, sizeof(conffile));
	}
	if(readconf(fd) < 0)
		prerror("readconf");
	close(fd);

	print("%s...", bootfile);
	while((fd = open(bootfile, OREAD)) < 0){
		outin("bootfile", bootfile, sizeof(bootfile));
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

	errstr(buf);
	fprint(2, "boot: %s: %s\n", s, buf);
}

/*
 *  print error and exit
 */
void
error(char *s)
{
	char buf[64];

	errstr(buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}

/*
 *  lookup the address for a system
 */
Address *
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
			return a;
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
		if(write(out, buf, n) != n){
			prerror("writing #b/mem");
			return -1;
		}
	}
	return 0;
}

/*
 *  set a configuration value
 */


/*
 *  read the configuration
 */
int
readconf(int fd)
{
	int bfd;
	int n;
	long x;

	/*
 	 *  read the config file
	 */
	n = read(fd, buf, sizeof(buf)-1);
	if(n <= 0)
		return -1;
	buf[n] = 0;

	/*
	 *  write into 4 meg - 4k
	 */
	bfd = open("#b/mem", OWRITE);
	if(bfd < 0){
		prerror("can't open #b/mem");
		return;
	}
	x = 0x80000000 | 4*1024*1024 - 4*1024;
	if(seek(bfd, x, 0) != x){
		close(bfd);
		return -1;
	}
	if(write(bfd, buf, n+1) != n+1){
		close(bfd);
		return -1;
	}

	close(bfd);
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
	print("+%d", a_out.bss);

	close(bfd);
	bfd = open("#b/boot", OWRITE);
	if(bfd < 0){
		prerror("can't open #b/boot");
		return;
	}
	
	print(" entry: 0x%ux\n", a_out.entryva);
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
outin(char *prompt, char *def, int len)
{
	int n;

	do{
		print("%s[%s]: ", prompt, def);
		n = read(0, buf, len);
	}while(n==0);
	if(n < 0)
		error("can't read #c/cons; please reboot");
	if(n != 1){
		buf[n-1] = 0;
		strcpy(def, buf);
	}
	return n;
}
