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
	int factotumpid, pid;

	/* start agent */
	ac = 0;
	av = argv;
	av[ac++] = "factotum";
//av[ac++] = "-dt";
	if(cpuflag)
		av[ac++] = "-s";
	else
		av[ac++] = "-u";
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
	/* ask for password */
#ifdef quux
	/* start agent */
	ac = 0;
	av = argv;
	av[ac++] = "factotum";
	av[ac++] = "-gd";
	av[ac++] = "p9sk1";
	av[ac++] = "cs.bell-labs.com";
	av[ac] = 0;
	switch((factotumpid = fork())){
	case -1:
		fatal("starting factotum: %r");
	case 0:
		exec("/factotum", av);
		fatal("execing /factotum");
	default:
		break;
	}

	for(;;){
		pid = waitpid();
		if(pid == factotumpid)
			break;
		if(pid == -1)
			fatal("waiting for factotum");
	}
#endif quux
}
