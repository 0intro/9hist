#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include "../boot/boot.h"

char	*authaddr;

void
authentication(int cpuflag)
{
	char *argv[16], **av;
	int ac;

	/* make capabilities available to factotum */
	bind("#¤", "/dev", MAFTER);

	/* start agent */
	ac = 0;
	av = argv;
	av[ac++] = "factotum";
//av[ac++] = "-d";
	if(cpuflag)
		av[ac++] = "-S";
	else
		av[ac++] = "-u";
	av[ac++] = "-sfactotum";
	if(authaddr != nil){
		av[ac++] = "-a";
		av[ac++] = authaddr;
	}
	av[ac] = 0;
	switch(fork()){
	case -1:
		fatal("starting factotum: %r");
	case 0:
		exec("/factotum", av);
		fatal("execing /factotum");
	default:
		break;
	}

	/* wait for agent to really be there */
	while(access("/mnt/factotum", 0) < 0)
		sleep(250);

	if(cpuflag)
		return;
}
