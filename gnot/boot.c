#include <u.h>
#include <libc.h>

#include "fcall.h"

Fcall	hdr;
char	buf[100];

void	error(char*);
void	sendmsg(int, char*);

main(int argc, char *argv[])
{
	int cfd, fd, n, fu, f;
	char buf[NAMELEN];
	int p[2];

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);

	/*
	 *  grab the incon,
	 *  push the dk multiplexor onto it,
	 *  and use line 1 as the signalling channel.
	 */
	cfd = open("#i/ctl", 2);
	if(cfd < 0)
		error("opening #i/ctl");
	sendmsg(cfd, "push dkmux");
	sendmsg(cfd, "config 1 16");

	/*
	 *  open a datakit channel and call ken via r70, leave the
	 *  incon ctl channel open
	 */
	fd = open("#k/2/data", 2);
	if(fd < 0)
		error("opening #k/2/data");
	cfd = open("#k/2/ctl", 2);
	if(cfd < 0)
		error("opening #k/2/ctl");
	sendmsg(cfd, "connect r70.nonet!bootes!fs");
	print("connected to r70.nonet!bootes!fs\n");
	close(cfd);

	/*
	 *  talk to the file server
	 */
	print("nop...");
	hdr.type = Tnop;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write nop");
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

	print("session...");
	hdr.type = Tsession;
	hdr.lang = 'v';
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write session");
	n = read(fd, buf, sizeof buf);
	if(n <= 0)
		error("read session");
	if(convM2S(buf, &hdr, n) == 0)
		error("format session");
	if(hdr.type != Rsession)
		error("not Rsession");
	if(hdr.err){
		print("error %d;", hdr.err);
		error("remote error");
	}

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
	
	print("mount...");
	if(bind("/", "/", MREPL) < 0)
		error("bind");
	if(mount(fd, "/", MAFTER|MCREATE, "") < 0)
		error("mount");
	print("success\n");
	execl("/68020/init", "init", 0);
	error("/68020/init");
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

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
