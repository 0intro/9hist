#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "../boot/boot.h"

static Fcall	hdr;
static char	buf[4*1024];

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
