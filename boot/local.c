#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

static char *disk;

void
configlocal(Method *mp)
{
	disk = mp->arg;
	USED(mp);
}

int
authlocal(void)
{
	return -1;
}

int
connectlocal(void)
{
	ulong i;
	int p[2];
	char d[DIRLEN];
	char sbuf[32];
	char rbuf[32];
	char *mtpt;
	char partition[2*NAMELEN];

	if(stat("/kfs", d) < 0)
		return -1;
	sprint(partition, "%sfs", mp->arg ? mp->arg : bootdisk);
	if(stat(partition, d) < 0)
		return -1;

	if(bind("#c", "/dev", MREPL) < 0)
		fatal("bind #c");
	if(bind("#p", "/proc", MREPL) < 0)
		fatal("bind #p");
	if(pipe(p)<0)
		fatal("pipe");
	switch(fork()){
	case -1:
		fatal("fork");
	case 0:
		sprint(sbuf, "%d", p[0]);
		sprint(rbuf, "%d", p[1]);
		execl("/kfs", "kfs", "-f", partition, "-s", sbuf, rbuf, 0);
		fatal("can't exec kfs");
	default:
		break;
	}

	close(p[1]);
	return p[0];
}
