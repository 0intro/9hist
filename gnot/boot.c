#include <u.h>
#include <libc.h>

#include "fcall.h"

Fcall	hdr;
char	buf[100];

void	error(char *);

main(int argc, char *argv[])
{
	int cfd, fd, n, fu, f;
	char buf[NAMELEN];
	int p[2];

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);

	do{
		print("user: ");
		n = read(0, buf, sizeof buf);
	}while(n==0 || n==1);
	if(n < 0)
		error("can't read #c/cons; please reboot");
	buf[n-1] = 0;
	print("hello %s!\n", buf);
	if(pipe(p) == -1)
		error("pipe");
	if(write(p[1], "hohoHO!", 8) != 8)
		error("write");
	if(read(p[0], buf, 8) != 8)
		error("read");
	print("%s\n", buf);
	for(;;);
}

void
error(char *s)
{
	char buf[64];

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
