#include <u.h>
#include <libc.h>
#include <fcall.h>

#define DEFSYS "bootes"
#define DEFFILE "/mips/9"

Fcall	hdr;
char	*scmd;

char	buf[4*1024];
char	bootfile[5*NAMELEN];
char	sys[NAMELEN];

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
	{ "bootes", "connect 080069020205" },
	{ "helix", "connect 080069020427" },
	{ "spindle", "connect 0800690202df" },
	{ "r70", "connect 08002b04265d" },
	{ 0 }
};

/*
 *  predeclared
 */
int	outin(char *, char *, int);
void	prerror(char *);
void	error(char *);
void	boot(int);
int	dkdial(char *);
int	nonetdial(char *);
int	bitdial(char *);

/*
 *  usage: 9b [-a] [server] [file]
 *
 *  default server is `bootes', default file is `/mips/9'
 */
main(int argc, char *argv[])
{
	int i;
	int manual=0;
	char buf[128];

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	i = create("#e/sysname", 1, 0666);
	if(i < 0)
		error("sysname");
	if(write(i, argv[0], strlen(argv[0])) != strlen(argv[0]))
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

	boot(manual);
	for(;;){
		if(fd > 0)
			close(fd);
		if(cfd > 0)
			close(cfd);
		fd = cfd = 0;
		boot(1);
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
		fd = cfd = -1;
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
	fd = open("#kdk/5/data", 2);
	if(fd < 0) {
		prerror("opening #kdk/5/data");
		return -1;
	}
	cfd = open("#kdk/5/ctl", 2);
	if(cfd < 0){
		close(fd);
		fd = -1;
		prerror("opening #kdk/5/ctl");
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
boot(int ask)
{
	int n, f, tries;
	char *srvname;

	if(ask){
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
	hdr.tag = ~0;
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
	if(hdr.tag != ~0){
		prerror("tag not ~0");
		return;
	}

	print("session...");
	hdr.type = Tsession;
	hdr.tag = ~0;
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
	if(hdr.tag != ~0){
		prerror("tag not ~0");
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

	print("post...");
	sprint(buf, "#s/%s", srvname);
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
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
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
 *  prompt and get input
 */
int
int
outin(char *prompt, char *def, int len)
{
	int n;
	char buf[256];

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
