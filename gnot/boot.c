#include <u.h>
#include <libc.h>
#include <fcall.h>

#define DEFSYS "Nfs"

enum
{
	CtrlD	= 4,
	Cr	= 13,
};

char	*net;
char	*netdev;

Fcall	hdr;
char	*scmd;
char 	bootdevice;
int	authenticated;

char	bootline[64];
char	bootuser[NAMELEN];
char	password[NAMELEN];
char	username[NAMELEN];
char	sys[NAMELEN];
char	buf[4*1024];

int format;
int manual;

/*
 *  predeclared
 */
void	bootparams(void);
int	outin(char *, char *, int);
void	prerror(char *);
void	error(char *);
int	dkdial(char *);
void	nop(int);
void	session(int);
int	cache(int);
void	sendmsg(int, char *);
void	connect(int);
void	kill(int);
void	passwd(void);
int	authenticate(int);
void	termtype(char*);
void	setuser(char*);
int	fileserver(void);
int	inconctl(void);
int	asyncctl(char*);
void	dkconfig(int);
void	boot(int);

/*
 * Ethernet type stations boot over ether or use dk via RS232.
 */
main(int argc, char *argv[])
{
	int cfd;
	int fd;


	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	sleep(1000);

	/*
	 *  get parameters passed by boot rom to kernel
	 */
	bootparams();
	termtype("at&t gnot 1");

	/*
	 *  user/passwd pair if the boot rom didn't
	 *  authenticate
	 */
	if(!authenticated){
		strcpy(username, "none");
		outin("user", username, sizeof(username));
		passwd();
	} else {
		strcpy(username, bootuser);
	}
	setuser(username);

	/*
	 *  get the control channel for the network
	 *  device
	 */
	switch(fileserver()){
	case 'a':
		cfd = asyncctl("B19200");
		break;
	case 'A':
		cfd = asyncctl("B9600");
		break;
	case 'i':
	default:
		cfd = inconctl();
		break;
	}

	/*
	 *  start up the datakit and connect to
	 *  file server
	 */
	dkconfig(cfd);
	for(;;){
		fd = dkdial(sys);
		if(fd >= 0)
			break;
		print("can't connect, retrying...\n");
		sleep(1000);
	}

	/*
	 *  set up the file system connection
	 */
	boot(fd);

	/*
	 *  go to init
	 */
	if(manual)
		execl("/68020/init", "init", "-m", 0);
	else
		execl("/68020/init", "init", 0);
	error("/68020/init");
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
	f = open("#e/bootuser", OREAD);
	if(f >= 0){
		read(f, &bootuser, sizeof(bootuser)-1);
		close(f);
	}
	f = open("#e/bootserver", OREAD);
	if(f >= 0){
		read(f, sys, sizeof(sys)-1);
		close(f);
	} else
		strcpy(sys, DEFSYS);

	/*
	 *  perhaps a stupid assumption
	 */
	if(bootdevice == 'i')
		authenticated = 1;
}

/*
 *  set flavor of terminal
 */
void
termtype(char *t)
{
	int fd;

	fd = create("#e/terminal", 1, 0666);
	if(fd < 0)
		error("terminal");
	if(write(fd, t, strlen(t)) < 0)
		error("terminal");
	close(fd);
}

/*
 *  get user and password if the
 *  boot rom didn't authenticate
 */
void
setuser(char *name)
{
	int fd;

	/*
	 *  set user id
	 */
	fd = open("#c/user", OWRITE|OTRUNC);
	if(fd >= 0){
		if(write(fd, username, strlen(username)) <= 0)
			print("error writing %s to /dev/user\n", username);
		close(fd);
	}
}

#define FS "(9)600 serial, (1)9200 serial, (i)incon"
/*
 *  if we've booted off the disk, figure out where to get the
 *  file service from
 */
int
fileserver(void)
{
	char reply[4];

	if(bootdevice != 's')
		return bootdevice;

	for(;;){
		strcpy(reply, "9");
		strcpy(sys, DEFSYS);
		outin(FS, reply, sizeof(reply));
		switch(reply[0]){
		case 'i':
			outin("server", sys, sizeof(sys));
			return 'i';
		case 'l':
			return 'l';
		case '1':
			outin("server", sys, sizeof(sys));
			return 'a';
		case '9':
			outin("server", sys, sizeof(sys));
			return 'A';
		}
	}
}

/*
 *  get the incon control channel
 */
int
inconctl(void)
{
	int cfd;

	cfd = open("#i/ctl", ORDWR);
	if(cfd < 0)
		error("opening #i/ctl");

	return cfd;
}

/*
 *  get the serial control channel and let the
 *  user connect to the TSM8
 */
int
asyncctl(char *baud)
{
	int cfd, dfd;
	char reply[4];

	cfd = open("#t/tty0ctl", ORDWR);
	if(cfd < 0)
		error("opening #t/tty0ctl");

	sendmsg(cfd, baud);

	dfd = open("#t/tty0", ORDWR);
	if(dfd < 0)
		error("opening #t/tty0");

	connect(dfd);
	close(dfd);
	sendmsg(cfd, "push async");
	return cfd;
}

/*
 *  configure the datakit
 */
void
dkconfig(int cfd)
{
	sendmsg(cfd, "push dkmux");
	if(authenticated)
		sendmsg(cfd, "config 1 16 norestart");
	else
		sendmsg(cfd, "config 1 16 restart");

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

int
dkdial(char *arg)
{
	int fd, cfd;
	int i;
	long n;

	sprint(buf, "connect %s", arg);
	n = strlen(buf);

	for(i = 0; ; i++){
		fd = open("#k/2/data", ORDWR);
		if(fd < 0)
			error("opening #k/2/data");
		cfd = open("#k/2/ctl", ORDWR);
		if(cfd < 0)
			error("opening #k/2/ctl");

		if(write(cfd, buf, n)==n && authenticate(fd)==0)
			break;
		if(i == 5)
			return -1;
		close(fd);
		close(cfd);
		sleep(500);
	}
	print("connected to %s\n", arg);
	sendmsg(cfd, "init");
	close(cfd);
	net = "dk";
	netdev = "#k";
	return fd;	
}

void
boot(int fd)
{
	int n, f;
	char *srvname;
	Dir dir;
	char dirbuf[DIRLEN];

	srvname = strrchr(sys, '/');
	if(srvname)
		srvname++;
	else
		srvname = sys;
	nop(fd);
	session(fd);
	fd = cache(fd);

	/*
	 *  stick handles to the file system
	 *  into /srv
	 */
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

	/*
	 *  make the root a union
	 */
	print("mount...");
	if(bind("/", "/", MREPL) < 0)
		error("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		error("mount");

	/*
	 *  set the time from the access time of the root
	 *  of the file server, accessible as /..
	 */
	print("time...");
	if(stat("/..", dirbuf) < 0)
		error("stat");
	convM2D(dirbuf, &dir);
	f = open("#c/time", OWRITE);
	sprint(dirbuf, "%ld", dir.atime);
	write(f, dirbuf, strlen(dirbuf));
	close(f);

	print("success\n");

	/*
	 *  put a generic network device into the namespace
	 */
	if(netdev){
		char buf[64];
		sprint(buf, "/net/%s", net);
		bind(netdev, buf, MREPL);
		bind(netdev, "/net/net", MREPL);
	}
	if(net){
		char buf[64];
		sprint(buf, "/lib/netaddr.%s", net);
		print("binding %s onto /lib/netaddr.net\n", buf);
		bind(buf, "/lib/netaddr.net", MREPL);
	}
}

/*
 * authenticate with r70
 */
int
authenticate(int fd)
{
	int n;

	for(;;) {
		n = read(fd, buf, sizeof(buf));
		if(n != 2){
			passwd();
			return -1;
		}
		buf[2] = '\0';
		if(strcmp(buf, "OK") == 0)
			return 0;
		else if(strcmp(buf, "CH") == 0) {
			sprint(buf, "%s\n%s\n", username, password);
			write(fd, buf, strlen(buf));	
		} else  if(strcmp(buf, "NO") == 0) {
			passwd();
			sprint(buf, "%s\n%s\n", username, password);
			write(fd, buf, strlen(buf));	
		}
	}
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
	hdr.tag = NOTAG;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write nop");
	n = read(fd, buf, sizeof buf);
	if(n==2 && buf[0]=='O' && buf[1]=='K')
		n = read(fd, buf, sizeof buf);
	if(n <= 0)
		error("read nop");
	if(convM2S(buf, &hdr, n) == 0) {
		print("n = %d; buf = %#.2x %#.2x %#.2x %#.2x\n",
			n, buf[0], buf[1], buf[2], buf[3]);
		error("format nop");
	}
	if(hdr.type != Rnop)
		error("not Rnop");
}

/*
 *  send session to file server
 */
void
session(int fd)
{
	long n;

	print("session...");
	hdr.type = Tsession;
	hdr.tag = NOTAG;
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
	ulong i;
	int p[2];
	Dir d;

	/*
	 *  if there's no /cfs, just return the fd to the
	 *  file server
	 */
	if(dirstat("/cfs", &d) < 0)
		return fd;
	if(dirstat("#r/hd0cache", &d) < 0)
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
			execl("/cfs", "bootcfs", "-fs", "-p", "#r/hd0cache", 0);
		else
			execl("/cfs", "bootcfs", "-s", "-p", "#r/hd0cache", 0);
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
void
connect(int fd)
{
	char xbuf[128];
	int i, pid, n, rcons;

	print("[ctrl-d to attach fs]\n");

	switch(pid = fork()) {
	case -1:
		error("fork failed");
	case 0:
		for(;;) {
			n = read(fd, xbuf, sizeof(xbuf));
			if(n < 0) {
				errstr(xbuf);
				print("[remote read error (%s)]\n", xbuf);
				for(;;);
			}
			for(i = 0; i < n; i++)
				if(xbuf[i] == Cr)
					xbuf[i] = ' ';
			write(1, xbuf, n);
		}
	default:
		rcons = open("#c/rcons", OREAD);
		if(rcons < 0)
			error("opening rcons");

		for(;;) {
			read(rcons, xbuf, 1);
			switch(xbuf[0]) {
			case CtrlD:
				kill(pid);
				close(rcons);
				return;
			default:
				n = write(fd, xbuf, 1);
				if(n < 0) {
					errstr(xbuf);
					kill(pid);
					close(rcons);
					print("[remote write error (%s)]\n", xbuf);
				}
			}
		}
	}
}

void
kill(int pid)
{
	char xbuf[32];
	int f;

	sprint(xbuf, "/proc/%d/note", pid);
	f = open(xbuf, OWRITE);
	write(f, "die", 3);
	close(f);
}

void
passwd(void)
{
	Dir d;
	char c;
	int i, n, fd, p[2];

	fd = open("#c/rcons", OREAD);
	if(fd < 0)
		error("can't open #c/rcons; please reboot");
 Prompt:		
	print("password: ");
	n = 0;
	do{
		do{
			i = read(fd, &c, 1);
			if(i < 0)
				error("can't read #c/rcons; please reboot");
		}while(i == 0);
		switch(c){
		case '\n':
			break;
		case '\b':
			if(n > 0)
				n--;
			break;
		case 'u' - 'a' + 1:		/* cntrl-u */
			print("\n");
			goto Prompt;
		default:
			password[n++] = c;
			break;
		}
	}while(c != '\n' && n < sizeof(password));
	password[n] = '\0';
	close(fd);
	print("\n");
}
