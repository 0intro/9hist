#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

void configip(char* dev);

// HACK - take over from boot since files system is not
// available on a pipe

void
configsac(Method *mp)
{
	int fd;
	char cmd[64];

	USED(mp);

print("configsac\n");
	/*
	 *  create the name space, mount the root fs
	 */
	if(bind("/", "/", MREPL) < 0)
		fatal("bind /");
	if(bind("#C", "/", MAFTER) < 0)
		fatal("bind /");

	fd = open("#c/sysname", OWRITE);
	if(fd < 0)
		fatal("open sysname");
	write(fd, "brick", 5);
	close(fd);

	fd = open("#c/hostowner", OWRITE);
	if(fd < 0)
		fatal("open sysname");
	write(fd, "brick", 5);
	close(fd);

	bargc = parsefields("-n 135.104.9.69 255.255.255.0 135.104.9.1 "
						"135.104.9.69 135.104.9.69", bargv, Nbarg-1, " ");
	configip("ether0");

	// fixed sysname - enables correct cpurc and namespace files
	// to be used

	sprint(cmd, "/%s/init", cputype);
	execl(cmd, "init", "-c", 0);
	fatal(cmd);
}

int
authsac(void)
{
	// does not get here
	return -1;
}

int
connectsac(void)
{
	// does not get here
	return -1;
}
