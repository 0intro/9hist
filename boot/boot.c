#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "../port/bootp.h"
#include "../port/arp.h"

#define DEFSYS "bootes"
typedef struct Net	Net;
typedef struct Flavor	Flavor;

enum
{
	Nterm	= 4,
	CtrlD	= 4,
	Cr	= 13,
	View	= 0x80,
};

Fcall	hdr;
int	printcol;

char	cputype[NAMELEN];
char	terminal[NAMELEN];
char	sys[2*NAMELEN];
char	username[NAMELEN];

int mflag;
int fflag;
int kflag;

void	nop(int);
void	session(int);
int	cache(int);
void	swapproc(void);
void	settime(int);
Method	*rootserver(char*);

void
main(int argc, char *argv)
{
	int fd;
	Method *mp;
	char cmd[64];
	char flags[5];
	int islocal;

	sleep(1000);

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);

	ARGBEGIN{
	case 'u':
		strcpy(username, ARGF());
		break;
	case 'k':
		kflag = 1;
		break;
	case 'm':
		mflag = 1;
		break;
	case 'f':
		fflag = 1;
		break;
	}ARGEND

	readenv("cputype", cputype, sizeof(cputype));
	readenv("terminal", terminal, sizeof(cputype));

	/*
	 *  pick a method and initialize it
	 */
	mp = rootserver(*argv);
	islocal = strcmp(mp->name, "local") == 0;
	(*mp->config)(mp);

	/*
	 *  get/set key or password
	 */
	(*pword)(mp);

	/*
	 *  connect to the root file system
	 */
	fd = (*mp->connect)();
	if(fd < 0)
		fatal("can't connect to file server");
	if(!islocal){
		nop(fd);
		session(fd);
		fd = cache(fd);
		srvcreate(sys, fd);
	}
	srvcreate("boot", fd);

	/*
	 *  create the name space, mount the root fs
	 */
	if(bind("/", "/", MREPL) < 0)
		fatal("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		fatal("mount");
	close(fd);

	/*
	 *  start local fs if its not the root file server
	 */
	if(!islocal){
		for(mp = method; mp->name; mp++)
			if(strcmp(mp->name, "local")==0){
				local = (*mp->connect)(mp);
				if(local < 0)
					break;
				if(mount(local, "/n/kfs", MAFTER|MCREATE, "", "") < 0)
					fatal("mount");
				close(local);
				break;
			}
	}

	settime(islocal);
	swapproc();

	sleep(1000);
	sprint(cmd, "/%s/init", cputype);
	sprint(flags, "-%s%s", cpuflag ? "c" : "t", mflag ? "m", "");
	execl(cmd, "init", flags, 0);
	fatal(cmd);
}

/*
 *  ask user from whence comes the root file system
 */
Method*
rootserver(char *arg)
{
	char prompt[256];
	char reply[64];
	Method *mp;
	char *cp;
	int n;

	mp = method;
	n = 0;
	sprint(prompt, "root is from (", mp->name);
	for(mp++; mp->name; mp++)
		n += sprint(prompt+n, ", %s", mp->name);
	sprint(prompt+n, ")");

	for(;;){
		strcpy(reply, method->name);
		if(arg == 0)
			outin(prompt, reply, sizeof(reply));
		else
			strcpy(reply, arg);
		arg = 0;
		for(mp = method; mp->name; mp++)
			if(*reply == *mp->name){
				cp = strchr(reply, '!');
				if(cp)
					strcpy(sys, cp+1);
				return mp;
			}
		if(mp->name == 0)
			continue;
	}
}

void
nop(int fd)
{
	long n;

	print("nop...");
	hdr.type = Tnop;
	hdr.tag = NOTAG;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		fatal("write nop");
	n = read(fd, buf, sizeof buf);
	if(n==2 && buf[0]=='O' && buf[1]=='K')
		n = read(fd, buf, sizeof buf);
	if(n <= 0)
		fatal("read nop");
	if(convM2S(buf, &hdr, n) == 0) {
		print("n = %d; buf = %#.2x %#.2x %#.2x %#.2x\n",
			n, buf[0], buf[1], buf[2], buf[3]);
		fatal("format nop");
	}
	if(hdr.type != Rnop)
		fatal("not Rnop");
}

void
session(int fd)
{
	long n;

	print("session...");
	hdr.type = Tsession;
	hdr.tag = NOTAG;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		fatal("write session");
	n = read(fd, buf, sizeof buf);
	if(n <= 0)
		fatal("read session");
	if(convM2S(buf, &hdr, n) == 0)
		fatal("format session");
	if(hdr.type == Rerror){
		print("error %s;", hdr.ename);
		fatal(hdr.ename);
	}
	if(hdr.type != Rsession)
		fatal("not Rsession");
}

int
cache(int fd)
{
	ulong i;
	int p[2];
	char d[DIRLEN];
	char partition[2*NAMELEN];

	if(stat("/cfs", d) < 0)
		return fd;
	sprint(partition, "%scache", bootdisk);
	if(stat(partition, d) < 0)
		return fd;
	print("cfs...");
	if(pipe(p)<0)
		fatal("pipe");
	switch(fork()){
	case -1:
		fatal("fork");
	case 0:
		close(p[1]);
		dup(fd, 0);
		close(fd);
		dup(p[0], 1);
		close(p[0]);
		if(format)
			execl("/cfs", "bootcfs", "-fs", "-p", partition, 0);
		else
			execl("/cfs", "bootcfs", "-s", "-p", partition, 0);
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
swapproc(void)
{
	int fd;

	fd = open("#c/swap", OWRITE);
	if(fd < 0){
		warning("opening #c/swap");
		return;
	}
	if(write(fd, "start", 5) <= 0)
		warning("starting swap kproc");
}

void
settime(int islocal)
{
	int n, f;
	int timeset;
	Dir dir;
	char dirbuf[DIRLEN];
	char *srvname;

	print("time...");
	timeset = 0;
	if(islocal){
		/*
		 *  set the time from the real time clock
		 */
		f = open("#r/rtc", ORDWR);
		if(f >= 0){
			if((n = read(f, dirbuf, sizeof(dirbuf)-1)) > 0){
				dirbuf[n] = 0;
				timeset = 1;
			}
			close(f);
		}
	}
	if(timeset == 0){
		/*
		 *  set the time from the access time of the root
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
			fatal("stat");
		convM2D(dirbuf, &dir);
		sprint(dirbuf, "%ld", dir.atime);
		unmount(0, "/n/boot");
		/*
		 *  set real time clock if there is one
		 */
		f = open("#r/rtc", ORDWR);
		if(f > 0){
			write(f, dirbuf, strlen(dirbuf));
			close(f);
		}
		close(f);
	}

	f = open("#c/time", OWRITE);
	write(f, dirbuf, strlen(dirbuf));
	close(f);
}
