#include <u.h>
#include <libc.h>
#include <fcall.h>

Fcall	hdr;
char	buf[100];
char	bootline[64];
char	bootdevice;
char	bootserver[64];

void	error(char*);
void	sendmsg(int, char*);

main(int argc, char *argv[])
{
	int cfd, fd, n, fu, f, i;
	char buf[256];
	int p[2];

	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);

	fd = open("#e/bootline", OREAD);
	if(fd >= 0){
		read(fd, bootline, sizeof bootline);
		close(fd);
	}
	fd = open("#e/bootdevice", OREAD);
	if(fd >= 0){
		read(fd, &bootdevice, 1);
		close(fd);
	}
	fd = open("#e/bootserver", OREAD);
	if(fd >= 0){
		read(fd, bootserver, 64);
		close(fd);
	} else
		strcpy(bootserver, "nfs");

	switch(bootdevice){
	case 'A':
		/*
		 *  grab the rs232 line,
		 *  make it 9600 baud,
		 *  push the async protocol onto it,
		 */
		cfd = open("#c/rs232ctl", 2);
		if(cfd < 0)
			error("opening #c/rs232ctl");
		sendmsg(cfd, "B9600");
		sendmsg(cfd, "push async");
		break;
	case 'a':
	case 's':
		/*
		 *  grab the rs232 line,
		 *  make it 19200 baud,
		 *  push the async protocol onto it,
		 */
		cfd = open("#c/rs232ctl", 2);
		if(cfd < 0)
			error("opening #c/rs232ctl");
		sendmsg(cfd, "B19200");
		sendmsg(cfd, "push async");
		break;
	default:
		/*
		 *  grab the incon,
		 */
		cfd = open("#i/ctl", 2);
		if(cfd < 0)
			error("opening #i/ctl");
		break;
	}

	/*
	 *  push the dk multiplexor onto the communications link,
	 *  and use line 1 as the signalling channel.
	 */
	sendmsg(cfd, "push dkmux");
	sendmsg(cfd, "config 1 16 norestart");

	/*
	 *  fork a process to hold the device channel open
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

	/*
	 *  open a datakit channel and call ken, leave the
	 *  incon ctl channel open
	 */
	for(i = 0; ; i++){
		fd = open("#k/2/data", 2);
		if(fd < 0)
			error("opening #k/2/data");
		cfd = open("#k/2/ctl", 2);
		if(cfd < 0)
			error("opening #k/2/ctl");
		sprint(buf, "connect %s", bootserver);
		n = strlen(buf);
		if(write(cfd, buf, n) == n)
			break;
		if(i == 5)
			error("dialing");
		print("error dialing, retrying ...\n");
		close(fd);
		close(cfd);
	}
	print("connected to %s\n", bootserver);
	close(cfd);

	/*
	 *  talk to the file server
	 */
	print("nop...");
	hdr.type = Tnop;
	hdr.tag = ~0;
	n = convS2M(&hdr, buf);
	if(write(fd, buf, n) != n)
		error("write nop");
	n = read(fd, buf, sizeof buf);
	if(n==2 && buf[0]=='O' && buf[1]=='K')
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
	hdr.tag = ~0;
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
	if(mount(fd, "/", MAFTER|MCREATE, "", "") < 0)
		error("mount");
	print("success\n");

	f = create("#e/bootnet", 1, 0666);
	if(f >= 0){
		if(write(f, "dk", 2) != 2)
			error("writing bootnet");
		close(f);
		if(bind("#kdk", "/net/dk", MREPL) < 0)
			error("binding bootnet");
	}

	if(strchr(bootline, ' '))
		execl("/68020/init", "init", "-m", 0);
	else
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

	errstr(buf);
	fprint(2, "boot: %s: %s\n", s, buf);
	exits(0);
}
