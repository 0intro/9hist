#include <u.h>
#include <libc.h>

#include <fcall.h>

#define DEFSYS "bitbootes"
#define DEFFILE "/mips/9"

Fcall	hdr;
char	*sys;
char	*scmd;
char	*bootfile;

char	sbuf[2*NAMELEN];
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

/*
 *  predeclared
 */
char	*lookup(char *);
int	outin(char *, char *, char *, int);
void	prerror(char *);
void	error(char *);
void	boot(int);

/*
 *  usage: 9b [-a] [server] [file]
 *
 *  default server is `bitbootes', default file is `/mips/9'
 */
main(int argc, char *argv[])
{
	int i;
	int manual=0;

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	i = create("#e/sysname", 1, 0666);
	if(i < 0)
		error("sysname");
	if(write(i, argv[0], strlen(argv[0])) <= 0)
		error("sysname");
	close(i);

	argv++;
	argc--;	

	while(argc > 0){
		if(argv[0][0] == '-'){
			if(argv[0][1] == 'm')
				manual = 1;
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

	boot(manual);
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
	int n, f;

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
	efd = open("#l/1/ctl", 2);
	if(efd < 0){
		prerror("opening #l/1/ctl");
		return;
	}
	if(write(efd, "connect 0x900", sizeof("connect 0x900")-1)<0){
		prerror("connect 0x900");
		return;
	}
	if(write(efd, "push noether", sizeof("push noether")-1)<0){
		prerror("push noether");
		return;
	}

	/*
	 *  grab a nonet channel and call up the ross file server
	 */
	fd = open("#n/2/data", 2);
	if(fd < 0) {
		prerror("opening #n/2/data");
		return;
	}
	cfd = open("#n/2/ctl", 2);
	if(cfd < 0){
		prerror("opening #n/2/ctl");
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
		print("n = %d\n", n);
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

	print("post...");
	sprint(buf, "#s/%s", sys);
	f = create(buf, 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);
	f = create("#s/boot", 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);

	print("mount...");
	if(bind("/", "/", MREPL) < 0)
		error("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "") < 0)
		error("mount");
	print("success\n");
	close(fd);

	if(ask)
		execl("/mips/init", "init", "-m", 0);
	else
		execl("/mips/init", "init", 0);
	error("/mips/init");
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
