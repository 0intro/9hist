#include <u.h>
#include <libc.h>

#include <fcall.h>

Fcall	hdr;
char	buf[100];
char	srv[100];

void	error(char *);

typedef
struct address {
	char *name;
	char *cmd;
} Address;

Address addr[] = {
	{ "bitbootes", "bitconnect" },
	{ "ross", "connect 020701005eff" },
	{ "bootes", "connect 080069020205" },
	{ "helix", "connect 080069020427" },
	{ "spindle", "connect 0800690202df" },
	{ "r70", "connect 08002b04265d" },
	{ 0 }
};

#define DEFUSER "bootes"

char *
lookup(char *arg)
{
	Address *a;

	if(strcmp(arg, "?")==0 || strcmp(arg, "help")==0){
		for(a = addr; a->name; a++)
			print("%s\n", a->name);
		return 0;
	}
	for(a = addr; a->name; a++){
		if(strcmp(a->name, arg) == 0)
			return a->cmd;
	}
	return 0;
}

main(int argc, char *argv[])
{
	int cfd, fd, n, fu, f;
	char buf[NAMELEN];
	char *scmd;

	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	/*
	 *  get server
	 */
	do{
		do{
			print("server[%s]: ", addr[0].name);
			n = read(0, srv, sizeof srv);
		}while(n==0);
		if(n < 0)
			error("can't read #c/cons; please reboot");
		if(n == 1)
			strcpy(srv, addr[0].name);
		else
			srv[n-1] = 0;
		scmd = lookup(srv);
	}while(scmd == 0);

	/*
	 *  get user.  if the user typed cr to the server question, skip
	 *  the user question and just use the default.
	 */
	if(n != 1){
		do{
			print("user[%s]: ", DEFUSER);
			n = read(0, buf, sizeof buf);
		}while(n==0);
		if(n < 0)
			error("can't read #c/cons; please reboot");
		if(n == 1)
			strcpy(buf, DEFUSER);
		else
			buf[n-1] = 0;
	}else
		strcpy(buf, DEFUSER);

	fu = create("#c/user", 1, 0600);
	if(fu < 0)
		error("#c/user");
	n = strlen(buf);
	if(write(fu, buf, n) != n)
		error("user write");
	close(fu);

	if(strcmp(scmd, "bitconnect") == 0){
		fd = open("#3/bit3", ORDWR);
		if(fd < 0)
			error("opening #3/bit3");
		goto Mesg;
	}

	/*
	 *  grab a lance channel, make it recognize ether type 0x900,
	 *  and push the nonet ethernet multiplexor onto it.
	 */
	cfd = open("#l/1/ctl", 2);
	if(cfd < 0)
		error("opening #l/1/ctl");
	if(write(cfd, "connect 0x900", sizeof("connect 0x900")-1)<0)
		error("connect 0x900");
	if(write(cfd, "push noether", sizeof("push noether")-1)<0)
		error("push noether");

	/*
	 *  grab a nonet channel and call up the ross file server
	 */
	fd = open("#n/1/data", 2);
	if(fd < 0)
		error("opening #n/1/data");
	cfd = open("#n/1/ctl", 2);
	if(cfd < 0)
		error("opening #n/1/ctl");
	if(write(cfd, scmd, strlen(scmd))<0)
		error(scmd);

    Mesg:
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
	sprint(buf, "#s/%s", srv);
	f = create(buf, 1, 0666);
	if(f < 0)
		error("create");
	sprint(buf, "%d", fd);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write");
	close(f);
	sprint(buf, "#s/%s", srv);
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
	execl("/mips/init", "init", 0);
	error("/mips/init");
}

void
error(char *s)
{
	char buf[64];

	errstr(0, buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
