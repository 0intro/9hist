#include <u.h>
#include <libc.h>
#include "../boot/boot.h"

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

typedef struct Net	Net;
typedef struct Flavor	Flavor;

int	printcol;

char	cputype[NAMELEN];
char	terminal[NAMELEN];
char	sys[2*NAMELEN];
char	username[NAMELEN];
char	conffile[2*NAMELEN];
char	sysname[2*NAMELEN];
char	buf[8*1024];

int mflag;
int fflag;
int kflag;

int	cache(int);
Method	*rootserver(char*);
int	readconf(int);
void	readkernel(int);
int	readseg(int, int, long, long, int);

/*
 *  default boot file
 */
#define DEFFILE "/mips/9power"

void
main(int argc, char *argv[])
{
	int fd;
	Method *mp;
	char cmd[64];
	char flags[6];
	int islocal;
	char *bootfile;

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
	readenv("sysname", sysname, sizeof(sysname));
	if(argc > 1)
		bootfile = argv[1];
	else
		bootfile = DEFFILE;

	/*
	 *  pick a method and initialize it
	 */
	mp = rootserver(argc ? *argv : 0);
	(*mp->config)(mp);
	islocal = strcmp(mp->name, "local") == 0;

	/*
	 *  connect to the root file system
	 */
	fd = (*mp->connect)();
	if(fd < 0)
		fatal("can't connect to file server");
	if(!islocal){
		nop(fd);
		session(fd);
		if(cfs)
			fd = (*cfs)(fd);
	}

	/*
	 *  create the name space, mount the root fs
	 */
	if(bind("/", "/", MREPL) < 0)
		fatal("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		fatal("mount");
	close(fd);

	/*
	 *  open the configuration file and read it
	 *  into the kernel
	 */
	sprint(conffile, "/mips/conf/%s", sysname);
	print("%s...", conffile);
	while((fd = open(conffile, OREAD)) < 0)
		outin("conffile", conffile, sizeof(conffile));
	if(readconf(fd) < 0)
		fatal("readconf");
	close(fd);

	/*
	 *  read in real kernel
	 */
	print("%s...", bootfile);
	while((fd = open(bootfile, OREAD)) < 0)
		outin("bootfile", bootfile, sizeof(bootfile));
	readkernel(fd);
	fatal("couldn't read kernel");
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
	int n;

	mp = method;
	n = sprint(prompt, "root is from (%s", mp->name);
	for(mp++; mp->name; mp++)
		n += sprint(prompt+n, ", %s", mp->name);
	sprint(prompt+n, ")");

	if(arg)
		strcpy(reply, arg);
	else
		strcpy(reply, method->name);
	for(;;){
		if(arg == 0 || mflag)
			outin(prompt, reply, sizeof(reply));
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
	if(bfd < 0)
		fatal("can't open #b/mem");
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
void
readkernel(int fd)
{
	int n;
	int bfd;

	bfd = open("#b/mem", OWRITE);
	if(bfd < 0)
		fatal("can't open #b/mem");

	n = read(fd, &a_out, sizeof(a_out));
	if(n <= 0)
		fatal("can't read boot file");

	print("\n%d", a_out.text);
	if(readseg(fd, bfd, 20*4, a_out.textva, a_out.text)<0)
		fatal("can't read boot file");
	print("+%d", a_out.data);
	if(readseg(fd, bfd, 20*4 + a_out.text, a_out.textva + a_out.text, a_out.data)<0)
		fatal("can't read boot file");
	print("+%d", a_out.bss);

	close(bfd);
	bfd = open("#b/boot", OWRITE);
	if(bfd < 0)
		fatal("can't open #b/boot");
	
	print(" entry: 0x%ux\n", a_out.entryva);
	sleep(1000);
	if(write(bfd, &a_out.entryva, sizeof a_out.entryva) != sizeof a_out.entryva)
		fatal("can't start kernel");
}

/*
 *  read a segment into memory
 */
int
readseg(int in, int out, long inoff, long outoff, int len)
{
	long	n, i;

	if(seek(in, inoff, 0) < 0){
		warning("seeking bootfile");
		return -1;
	}
	if(seek(out, outoff, 0) != outoff){
		warning("seeking #b/mem");
		return -1;
	}
	for(; len > 0; len -= n){
		if((n = read(in, buf, sizeof buf)) <= 0){
			warning("reading bootfile");
			return -1;
		}
		if(write(out, buf, n) != n){
			warning("writing #b/mem");
			return -1;
		}
	}
	return 0;
}

