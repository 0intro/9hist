#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include "../boot/boot.h"

char	cputype[64];
char	sys[2*64];
char 	reply[256];
int	printcol;
int	mflag;
int	fflag;
int	kflag;

char	*bargv[Nbarg];
int	bargc;

static void	swapproc(void);
static Method	*rootserver(char*);

static int
rconv(va_list *arg, Fconv *fp)
{
	char s[ERRMAX];

	USED(arg);

	s[0] = '\0';
	errstr(s, sizeof s);
	strconv(s, fp);
	return 0;
}

void
boot(int argc, char *argv[])
{
	int fd;
	Method *mp;
	char cmd[64];
	char rootbuf[64];
	char flags[6];
	int islocal, ishybrid;
	char *rp;
	int n;
	char buf[32];

	sleep(1000);

	fmtinstall('r', rconv);

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	bind("#c", "/dev", MAFTER);
	bind("#e", "/env", MREPL|MCREATE);

#ifdef DEBUG
	print("argc=%d\n", argc);
	for(fd = 0; fd < argc; fd++)
		print("%lux %s ", argv[fd], argv[fd]);
	print("\n");
#endif DEBUG

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

	readfile("#e/cputype", cputype, sizeof(cputype));

	/*
	 *  pick a method and initialize it
	 */
	mp = rootserver(argc ? *argv : 0);
	(*mp->config)(mp);
	islocal = strcmp(mp->name, "local") == 0;
	ishybrid = strcmp(mp->name, "hybrid") == 0;

	/*
	 *  get/set key or password
	 */
	(*pword)(islocal, mp);

	/*
	 *  connect to the root file system
	 */
	fd = (*mp->connect)();
	if(fd < 0)
		fatal("can't connect to file server");
	if(getenv("srvold9p"))
		fd = old9p(fd);
	print("version...");
	buf[0] = '\0';
	n = fversion(fd, 0, buf, sizeof buf);
	if(n < 0)
		fatal("can't init 9P");
	print("(%.*s)...", n, buf);
	if(!islocal && !ishybrid){
		if(cfs)
			fd = (*cfs)(fd);
		doauthenticate(fd, mp);
	}
	srvcreate("boot", fd);

	/*
	 *  create the name space, mount the root fs
	 */
	if(bind("/", "/", MREPL) < 0)
		fatal("bind /");
	rp = getenv("rootspec");
	if(rp == nil)
		rp = "";
	if(mount(fd, "/root", MREPL|MCREATE, rp) < 0)
		fatal("mount /");
	rp = getenv("rootdir");
	if(rp == nil)
		rp = rootdir;
	if(bind(rp, "/", MAFTER|MCREATE) < 0){
		if(strncmp(rp, "/root", 5) == 0){
			fprint(2, "boot: couldn't bind $rootdir=%s to root: %r\n", rp);
			fatal("second bind /");
		}
		snprint(rootbuf, sizeof rootbuf, "/root/%s", rp);
		rp = rootbuf;
		if(bind(rp, "/", MAFTER|MCREATE) < 0){
			fprint(2, "boot: couldn't bind $rootdir=%s to root: %r\n", rp);
			if(strcmp(rootbuf, "/root//plan9") == 0){
				fprint(2, "**** warning: remove rootdir=/plan9 entry from plan9.ini\n");
				rp = "/root";
				if(bind(rp, "/", MAFTER|MCREATE) < 0)
					fatal("second bind /");
			}else
				fatal("second bind /");
		}
	}
	close(fd);
	setenv("rootdir", rp);

	/*
	 *  if a local file server exists and it's not
	 *  running, start it and mount it onto /n/kfs
	 */
	if(0 && access("#s/kfs", 0) < 0){	/* BUG: DISABLED UNTIL KFS SUPPORTS 9P2000 */
		for(mp = method; mp->name; mp++){
			if(strcmp(mp->name, "local") != 0)
				continue;
			(*mp->config)(mp);
			fd = (*mp->connect)();
			if(fd < 0)
				break;
			mount(fd, "/n/kfs", MAFTER|MCREATE, "") ;
			close(fd);
			break;
		}
	}

	settime(islocal);
	swapproc();

	sprint(cmd, "/%s/ninit", cputype);
	sprint(flags, "-%s%s", cpuflag ? "c" : "t", mflag ? "m" : "");
	execl(cmd, "init", flags, 0);
	fatal(cmd);
}

Method*
findmethod(char *a)
{
	Method *mp;
	int i, j;
	char *cp;

	i = strlen(a);
	cp = strchr(a, '!');
	if(cp)
		i = cp - a;
	for(mp = method; mp->name; mp++){
		j = strlen(mp->name);
		if(j > i)
			j = i;
		if(strncmp(a, mp->name, j) == 0)
			break;
	}
	if(mp->name)
		return mp;
	return 0;
}

/*
 *  ask user from whence cometh the root file system
 */
static Method*
rootserver(char *arg)
{
	char prompt[256];
	Method *mp;
	char *cp;
	int n;

	/* make list of methods */
	mp = method;
	n = sprint(prompt, "root is from (%s", mp->name);
	for(mp++; mp->name; mp++)
		n += sprint(prompt+n, ", %s", mp->name);
	sprint(prompt+n, ")");

	/* create default reply */
	readfile("#e/bootargs", reply, sizeof(reply));
	if(reply[0] == 0 && arg != 0)
		strcpy(reply, arg);
	if(reply[0]){
		mp = findmethod(reply);
		if(mp == 0)
			reply[0] = 0;
	}
	if(reply[0] == 0)
		strcpy(reply, method->name);

	/* parse replies */
	for(;;){
		outin(prompt, reply, sizeof(reply));
		mp = findmethod(reply);
		if(mp){
			bargc = getfields(reply, bargv, Nbarg-1, 1, " ");
			cp = strchr(reply, '!');
			if(cp)
				strcpy(sys, cp+1);
			return mp;
		}
	}

	return 0;		/* not reached */
}

static void
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
	close(fd);
}

int
old9p(int fd)
{
	int p[2];

	if(pipe(p) < 0)
		fatal("pipe");

	print("srvold9p...");
	switch(fork()) {
	case -1:
		fatal("rfork srvold9p");
	case 0:
		dup(fd, 1);
		close(fd);
		dup(p[0], 0);
		close(p[0]);
		close(p[1]);
		execl("/srvold9p", "srvold9p", "-s", 0);
		fatal("exec srvold9p");
	default:
		close(fd);
		close(p[0]);
	}
	return p[1];	
}
