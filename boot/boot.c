#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include "../boot/boot.h"

typedef struct Net	Net;
typedef struct Flavor	Flavor;

char	cputype[NAMELEN];
char	terminal[NAMELEN];
char	sys[2*NAMELEN];
char	username[NAMELEN];
char	bootfile[3*NAMELEN];
char	conffile[NAMELEN];
int	printcol;
int	mflag;
int	fflag;
int	kflag;
int	aflag;
int	pflag;
int	afd = -1;
static void	swapproc(void);
static void	recover(Method*);
static Method	*rootserver(char*);

void
ethertest(void)
{
	int cf, df, n, t;
	char buf[64];
	struct Etherpkt
	{
		uchar	d[6];
		uchar	s[6];
		uchar	type[2];
	} p;

	cf = open("#l/ether/clone", ORDWR);
	if(cf < 0){
		print("can't open #l/ether/clone: %r\n");
		exits(0);
	}
	n = read(cf, buf, 12);
	if(n < 0){
		print("can't read #l/ether/clone: %r\n");
		exits(0);
	}
	buf[n] = 0;
	sprint(buf, "#l/ether/%d/data", atoi(buf));
	df = open(buf, ORDWR);
	if(df < 0){
		print("can't open %s: %r\n", buf);
		exits(0);
	}
	if(write(cf, "connect -1", sizeof("connect -1")-1) < 0){
		print("can't connect -1: %r\n");
		exits(0);
	}
	close(cf);
	for(;;){
		n = read(df, &p, sizeof(p));
		if(n <= 0){
			print("read returns %d: %r\n", n);
			continue;
		}
		t = (p.type[0]<<8) | p.type[1];
		print("%d %2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux -> %2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux %ux\n", n, p.s[0], p.s[1], p.s[2], p.s[3], p.s[4], p.s[5], p.d[0], p.d[1], p.d[2], p.d[3], p.d[4], p.d[5], t);
	}
}

void
boot(int argc, char *argv[])
{
	int fd;
	Method *mp;
	char cmd[64];
	char flags[6];
	int islocal, ishybrid;

	sleep(1000);

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	print("argc=%d\n", argc);
	for(fd = 0; fd < argc; fd++)
		print("%s ", argv[fd]);
	print("\n");/**/

	ethertest();

	if(argc <= 1)
		pflag = 1;

	ARGBEGIN{
	case 'a':
		aflag = 1;
		break;
	case 'u':
		strcpy(username, ARGF());
		break;
	case 'k':
		kflag = 1;
		break;
	case 'm':
		pflag = 1;
		mflag = 1;
		break;
	case 'p':
		pflag = 1;
		break;
	case 'f':
		fflag = 1;
		break;
	}ARGEND

	readfile("#e/cputype", cputype, sizeof(cputype));
	readfile("#e/terminal", terminal, sizeof(cputype));
	getconffile(conffile, terminal);

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

	switch(rfork(RFPROC|RFNAMEG|RFFDG)) {
	case -1:
		print("failed to start recover: %r\n");
		break;
	case 0:
		recover(mp);
		break;
	}

	/*
	 *  connect to the root file system
	 */
	fd = (*mp->connect)();
	if(fd < 0)
		fatal("can't connect to file server");
	nop(fd);
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
		fatal("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "") < 0)
		fatal("mount");
	close(fd);
	if(cpuflag == 0)
		newkernel();

	/*
	 *  if a local file server exists and it's not
	 *  running, start it and mount it onto /n/kfs
	 */
	if(access("#s/kfs", 0) < 0){
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
	close(afd);
	swapproc();

	sprint(cmd, "/%s/init", cputype);
	sprint(flags, "-%s%s%s", cpuflag ? "c" : "t", mflag ? "m" : "", aflag ? "a" : "");
	execl(cmd, "init", flags, 0);
	fatal(cmd);
}

/*
 *  ask user from whence cometh the root file system
 */
Method*
rootserver(char *arg)
{
	char prompt[256];
	char reply[64];
	Method *mp;
	char *cp;
	int n, j;
	int notfirst;

	mp = method;
	n = sprint(prompt, "root is from (%s", mp->name);
	for(mp++; mp->name; mp++)
		n += sprint(prompt+n, ", %s", mp->name);
	sprint(prompt+n, ")");

	if(arg)
		strcpy(reply, arg);
	else
		strcpy(reply, method->name);
	for(notfirst = 0;; notfirst = 1){
		if(pflag || notfirst)
			outin(prompt, reply, sizeof(reply));
		cp = strchr(reply, '!');
		if(cp)
			j = cp - reply;
		else
			j = strlen(reply);
		for(mp = method; mp->name; mp++)
			if(strncmp(reply, mp->name, j) == 0){
				if(cp)
					strcpy(sys, cp+1);
				return mp;
			}
		if(mp->name == 0)
			continue;
	}
	return 0;		/* not reached */
}

int
nop(int fd)
{
	int n;
	Fcall hdr;
	char buf[128];

	print("boot: nop...");
	hdr.type = Tnop;
	hdr.tag = NOTAG;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n){
		fatal("write nop");
		return 0;
	}
reread:
	n = read(fd, buf, sizeof buf);
	if(n <= 0){
		fatal("read nop");
		return 0;
	}
	if(n == 2)
		goto reread;
	if(convM2S(buf, &hdr, n) == 0) {
		fatal("format nop");
		return 0;
	}
	if(hdr.type != Rnop){
		fatal("not Rnop");
		return 0;
	}
	if(hdr.tag != NOTAG){
		fatal("tag not NOTAG");
		return 0;
	}
	return 1;
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

void
reattach(int rec, Method *amp, char *buf)
{
	char *mp;
	int fd, n, sv[2];
	char tmp[64], *p;

	mp = strchr(buf, ' ');
	if(mp == 0)
		goto fail;
	*mp++ = '\0';

	p = strrchr(buf, '/');
	if(p == 0)
		goto fail;
	*p = '\0';

	sprint(tmp, "%s/remote", buf);
	fd = open(tmp, OREAD);
	if(fd < 0)
		goto fail;
	n = read(fd, tmp, sizeof(tmp));
	if(n < 0)
		goto fail;
	close(fd);
	tmp[n-1] = '\0';

	print("boot: Service %s down, wait...\n", tmp);

	p = strrchr(buf, '/');
	if(p == 0)
		goto fail;
	*p = '\0';

	while(plumb(buf, tmp, sv, 0) < 0)
		sleep(30);

	nop(sv[1]);
	doauthenticate(sv[1], amp);

	print("boot: Service %s Ok\n", tmp);

	n = sprint(tmp, "%d %s", sv[1], mp);
	if(write(rec, tmp, n) < 0) {
		errstr(tmp);
		print("write recover: %s\n", tmp);
	}
	exits(0);
fail:
	print("recover fail: %s\n", buf);
	exits(0);
}

void
recover(Method *mp)
{
	int fd, n;
	char buf[256];

	fd = open("#/./recover", ORDWR);
	if(fd < 0)
		exits(0);

	for(;;) {
		n = read(fd, buf, sizeof(buf));
		if(n < 0)
			exits(0);
		buf[n] = '\0';

		if(fork() == 0)
			reattach(fd, mp, buf);
	}
}
