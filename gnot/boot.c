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
int	inconfs(void);
int	asyncfs(char*);
int	localfs(int);
void	dkconfig(int);
void	srvcreate(char*, int);
void	settime(int);

/*
 * Ethernet type stations boot over ether or use dk via RS232.
 */
main(int argc, char *argv[])
{
	int fd;
	int fromserver;

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	termtype("at&t gnot 1");

	/*
	 *  get parameters passed by boot rom to kernel
	 */
	bootparams();

	/*
	 *  prompt for user if the boot rom didn't authenticate
	 */
	if(!authenticated){
		strcpy(username, "none");
		outin("user", username, sizeof(username));
	} else {
		strcpy(username, bootuser);
	}
	setuser(username);

	/*
	 *  make the root a union
	 */
	if(bind("/", "/", MREPL) < 0)
		error("bind");

	/*
	 *  connect to file systems
	 */
	switch(fileserver()){
	case 'a':
		asyncfs("B19200");
		localfs(0);
		fromserver = 1;
		break;
	case 'A':
		asyncfs("B9600");
		localfs(0);
		fromserver = 1;
		break;
	case 'i':
		inconfs();
		localfs(0);
		fromserver = 1;
		break;
	default:
		localfs(1);
		fromserver = 0;
		break;
	}
	settime(fromserver);

	/*
	 *  put a default net into the name space
	 */
	if(netdev){
		sprint(buf, "/net/%s", net);
		bind(netdev, buf, MREPL);
		bind(netdev, "/net/net", MREPL);
	}
	if(net){
		sprint(buf, "/lib/netaddr.%s", net);
		bind(buf, "/lib/netaddr.net", MREPL);
	}

	if(manual)
		execl("/68020/init", "init", "-m", 0);
	else
		execl("/68020/init", "init", 0);
	error("/68020/init");
}

/*
 *  read arguments passed by kernel as
 *  environment variables
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
		read(f, &bootuser, sizeof(bootuser));
		close(f);
	}
	f = open("#e/bootserver", OREAD);
	if(f >= 0){
		read(f, sys, sizeof(sys));
		close(f);
	} else
		strcpy(sys, DEFSYS);
	if(bootdevice != 's')
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
		write(fd, username, strlen(username));
		close(fd);
	}
}

#define FS "remote fs is (9)600 serial, (1)9200 serial, (n)ot used"
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
		strcpy(reply, "n");
		strcpy(sys, DEFSYS);
		outin(FS, reply, sizeof(reply));
		switch(reply[0]){
		case 'i':
			return 'i';
		case 'n':
			return 'n';
		case '1':
			return 'a';
		case '9':
			return 'A';
		}
	}
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
	sendmsg(cfd, "init");
	close(cfd);
	net = "dk";
	netdev = "#k";
	return fd;	
}

/*
 *  connect to a file system over the serial line
 */
int
asyncfs(char *baud)
{
	int fd, cfd, dfd;
	char reply[4];

	if(!authenticated){
		passwd();
		outin("server", sys, sizeof(sys));
	}

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

	dkconfig(cfd);
	for(;;){
		fd = dkdial(sys);
		if(fd >= 0)
			break;
		print("can't connect, retrying...\n");
		sleep(1000);
	}

	nop(fd);
	session(fd);
	fd = cache(fd);
	srvcreate(sys, fd);
	srvcreate("boot", fd);
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		error("mount");
	close(fd);

	return 0;
}

/*
 *  connect to a file system over the incon
 */
int
inconfs(void)
{
	int fd, cfd, dfd;
	char reply[4];

	if(!authenticated){
		passwd();
		outin("server", sys, sizeof(sys));
	}
	cfd = open("#i/ctl", ORDWR);
	if(cfd < 0)
		error("opening #i/ctl");

	dkconfig(cfd);
	for(;;){
		fd = dkdial(sys);
		if(fd >= 0)
			break;
		print("can't connect, retrying...\n");
		sleep(1000);
	}

	nop(fd);
	session(fd);
	fd = cache(fd);
	srvcreate(sys, fd);
	srvcreate("boot", fd);
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		error("mount");
	close(fd);

	return 0;
}

/*
 *  plug the local file system into the name space
 */
int
localfs(int bootfs)
{
	ulong i;
	int p[2];
	Dir d;
	char sbuf[32];
	char rbuf[32];
	char *mtpt;

	if(dirstat("/kfs", &d) < 0)
		return -1;
	if(dirstat("#w/hd0fs", &d) < 0)
		return -1;

	print("local\n");

	/*
	 *  because kfs uses /dev/time, /dev/pid, and /proc/#
	 */
	if(bind("#c", "/dev", MREPL) < 0)
		error("bind #c");
	if(bind("#p", "/proc", MREPL) < 0)
		error("bind #p");

	if(pipe(p)<0)
		error("pipe");
	switch(fork()){
	case -1:
		error("fork");
	case 0:
		sprint(sbuf, "%d", p[0]);
		sprint(rbuf, "%d", p[1]);
		execl("/kfs", "kfs", "-f", "#w/hd0fs", "-s", sbuf, rbuf, 0);
		error("can't exec kfs");
	default:
		break;
	}

	close(p[1]);

/*
 *  NEW KERNEL - these can both be MAFTER
 */
	if(bootfs){
		mtpt = "/";
		if(mount(p[0], mtpt, MAFTER|MCREATE, "", "") < 0)
			error("mount");
		srvcreate("boot", p[0]);
	} else {
		mtpt = "/n/kfs";
		if(mount(p[0], mtpt, MREPL|MCREATE, "", "") < 0)
			error("mount");
	}
	
	close(p[0]);

	return 0;
}

void
srvcreate(char *name, int fd)
{
	char *srvname;
	int f;

	srvname = strrchr(name, '/');
	if(srvname)
		srvname++;
	else
		srvname = name;

	sprint(buf, "#s/%s", srvname);
	f = create(buf, 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);
}

/*
 *  set the system time
 */
void
settime(int fromserver)
{
	int n, f;
	Dir dir;
	char dirbuf[DIRLEN];
	char *srvname;

	print("time...");
	if(!fromserver){
		/*
		 *  set the time from the real time clock or file system
		 */
		f = open("#r/rtc", ORDWR);
		if(f > 0){
			if((n = read(f, dirbuf, sizeof(dirbuf)-1)) < 0)
				error("reading rtc");
			dirbuf[n] = 0;
			close(f);
		} else
			fromserver = 1;
	}
	if(fromserver){
		/*
		 *  set the time from the access time of the root
		 *  of the file server
		 */
		f = open("#s/boot", ORDWR);
		if(f < 0)
			return;
		if(mount(f, "/n/boot", MREPL, "", "") < 0){
			close(f);
			return;
		}
		close(f);
		if(stat("/n/boot", dirbuf) < 0)
			error("stat");
		convM2D(dirbuf, &dir);
		sprint(dirbuf, "%ld", dir.atime);
/*		unmount(0, "/n/boot"); /**/
		/*
		 *  set real time clock if there is one
		 */
		f = open("#r/rtc", ORDWR);
		if(f > 0){
			if(write(f, dirbuf, strlen(dirbuf)) < 0)
				error("writing rtc");
			close(f);
		}
		close(f);
	}
	f = open("#c/time", OWRITE);
	write(f, dirbuf, strlen(dirbuf));
	close(f);
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
	if(dirstat("#w/hd0cache", &d) < 0)
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
			execl("/cfs", "bootcfs", "-fs", "-p", "#w/hd0cache", 0);
		else
			execl("/cfs", "bootcfs", "-s", "-p", "#w/hd0cache", 0);
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
