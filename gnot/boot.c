#include <u.h>
#include <libc.h>
#include <fcall.h>

Fcall	hdr;
char	buf[100];
char	bootline[64];
char	bootdevice;
char	bootserver[64];
int	format;
int	manual;

void	error(char*);
void	sendmsg(int, char*);
void	bootparams(void);
void	dkconfig(void);
int	dkdial(void);
void	nop(int);
void	session(int);
int	cache(int);

main(int argc, char *argv[])
{
	int fd, f;
	char buf[256];
	Dir dir;

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);

	bootparams();
	dkconfig();
	fd = dkdial();
	nop(fd);
	session(fd);
	fd = cache(fd);

	/*
	 *  make a /srv/boot and a /srv/bootes
	 */
	print("post...");
	sprint(buf, "#s/%s", "bootes");
	f = create(buf, 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);
	sprint(buf, "#s/%s", "bootes");
	f = create("#s/boot", 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);

	/*
	 *  mount file server root after #/ root
	 */
	if(bind("/", "/", MREPL) < 0)
		error("bind");
	print("mount...");
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		error("mount");

	/*
	 * set the time from the access time of the root of the file server,
	 * accessible as /..
	 */
	print("time...");
	if(stat("/..", buf) < 0)
		error("stat");
	convM2D(buf, &dir);
	f = open("#c/time", OWRITE);
	sprint(buf, "%ld", dir.atime);
	write(f, buf, strlen(buf));
	close(f);
	
	print("success\n");

	bind("#k", "/net/net", MREPL);
	bind("#k", "/net/dk", MREPL);

	if(manual)
		execl("/68020/init", "init", "-m", 0);
	else {
		switch(fork()){
		case -1:
			print("can't start connection server\n");
			break;
		case 0:
			execl("/68020/init", "init", "-d", "/bin/cs", 0);
			error("/68020/bin/cs");
			break;
		default:
			execl("/68020/init", "init", 0);
		}
	}
	error("/68020/init");
}

/*
 *  open the network device, push on the needed multiplexors
 */
void
dkconfig(void)
{
	int cfd;

	switch(bootdevice){
	case 'A':
		/*
		 *  grab the rs232 line,
		 *  make it 9600 baud,
		 *  push the async protocol onto it,
		 */
		cfd = open("#c/rs232ctl", 2);
		if(cfd < 0)
			error("opening #c/rs232ctl");
		sendmsg(cfd, "B9600");
		sendmsg(cfd, "push async");
		break;
	case 'a':
	case 's':
		/*
		 *  grab the rs232 line,
		 *  make it 19200 baud,
		 *  push the async protocol onto it,
		 */
		cfd = open("#c/rs232ctl", 2);
		if(cfd < 0)
			error("opening #c/rs232ctl");
		sendmsg(cfd, "B19200");
		sendmsg(cfd, "push async");
		break;
	default:
		/*
		 *  grab the incon,
		 */
		cfd = open("#i/ctl", 2);
		if(cfd < 0)
			error("opening #i/ctl");
		break;
	}

	/*
	 *  push the dk multiplexor onto the communications link,
	 *  and use line 1 as the signalling channel.
	 */
	sendmsg(cfd, "push dkmux");
	sendmsg(cfd, "config 1 16 norestart");

	/*
	 *  fork a process to hold the device channel open
	 *  forever
	 */
	switch(fork()){
	case -1:
		break;
	case 0:
		for(;;)
			sleep(60*1000);
		exit(0);
	default:
		close(cfd);
		break;
	}
}

/*
 *  open a datakit channel and call ken, return an fd to the
 *  connection.
 */
int
dkdial(void)
{
	int fd, cfd;
	int i;
	long n;

	for(i = 0; ; i++){
		fd = open("#k/2/data", 2);
		if(fd < 0)
			error("opening #k/2/data");
		cfd = open("#k/2/ctl", 2);
		if(cfd < 0)
			error("opening #k/2/ctl");
		sprint(buf, "connect %s", bootserver);
		n = strlen(buf);
		if(write(cfd, buf, n) == n)
			break;
		if(i == 5)
			error("dialing");
		print("error dialing %s, retrying ...\n", bootserver);
		close(fd);
		close(cfd);
	}
	print("connected to %s\n", bootserver);
	close(cfd);
	return fd;
}

/*
 *  read arguments passed by kernel as
 *  environment variables - YECH!
 */
void
bootparams(void)
{
	int f;
	char *cp;

	format = 0;
	manual = 0;
	f = open("#e/bootline", OREAD);
	if(f >= 0){
		read(f, bootline, sizeof(bootline)-1);
		close(f);
		cp = bootline;
		while(cp = strchr(cp, ' ')){
			if(*++cp != '-')
				continue;
			while(*cp && *cp!=' ')
				switch(*cp++){
				case 'f':
					format = 1;
					break;
				case 'm':
					manual = 1;
					break;
				}
		}
	}
	f = open("#e/bootdevice", OREAD);
	if(f >= 0){
		read(f, &bootdevice, 1);
		close(f);
	}
	f = open("#e/bootserver", OREAD);
	if(f >= 0){
		read(f, bootserver, 64);
		close(f);
	} else
		strcpy(bootserver, "nfs");
}

/*
 *  send nop to file server
 */
void
nop(int fd)
{
	long n;

	print("nop...");
	hdr.type = Tnop;
	hdr.tag = ~0;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write nop");
	n = read(fd, buf, sizeof buf);
	if(n==2 && buf[0]=='O' && buf[1]=='K')
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
}

/*
 *  send nop to file server
 */
void
session(int fd)
{
	long n;

	print("session...");
	hdr.type = Tsession;
	hdr.tag = ~0;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write session");
	n = read(fd, buf, sizeof buf);
	if(n <= 0)
		error("read session");
	if(convM2S(buf, &hdr, n) == 0)
		error("format session");
	if(hdr.type == Rerror){
		print("error %s;", hdr.ename);
		error(hdr.ename);
	}
	if(hdr.type != Rsession)
		error("not Rsession");
}

/*
 *  see if we have a cache file system server in the kernel,
 *  and use it if we do
 */
int
cache(int fd)
{
	int f;
	ulong i;
	int p[2];

	/*
	 *  if there's no /cfs, just return the fd to the
	 *  file server
	 */
	f = open("/cfs", OREAD);
	if(f < 0)
		return fd;
	print("cfs...");

	/*
	 *  if we have a cfs, give it the file server as fd 0
	 *  and requests on fd 1
	 */
	if(pipe(p)<0)
		error("pipe");
	switch(fork()){
	case -1:
		error("fork");
	case 0:
		close(p[1]);
		dup(fd, 0);
		close(fd);
		dup(p[0], 1);
		close(p[0]);
		if(format)
			execl("/cfs", "bootcfs", "-fs", 0);
		else
			execl("/cfs", "bootcfs", "-s", 0);
		break;
	default:
		close(p[0]);
		close(fd);
		fd = p[1];
		break;
	}
	return fd;
}

void
sendmsg(int fd, char *msg)
{
	int n;

	n = strlen(msg);
	if(write(fd, msg, n) != n)
		error(msg);
}

void
error(char *s)
{
	char buf[64];

	errstr(buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
