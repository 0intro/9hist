#include <u.h>
#include <libc.h>
#include <auth.h>
#include <../boot/boot.h>

char defpass[] = {0x64, 0x3e, 0x4d, 0x13, 0x32, 0x00, 0x0b, 0xe1, 0xce};

void
key(int islocal, Method *mp)
{
	Nvrsafe safe;
	char password[20];
	int prompt, fd, i;

	USED(islocal);
	USED(mp);

	prompt = kflag;
	fd = open("#r/nvram", ORDWR);
	if(fd < 0){
		prompt = 1;
		warning("can't open nvram");
	}
	if(seek(fd, 1024+900, 0) < 0
	|| read(fd, &safe, sizeof safe) != sizeof safe)
		warning("can't read nvram key");

	if(prompt){
		do
			getpasswd(password, sizeof password);
		while(!passtokey(safe.machkey, password));
	}else if(nvcsum(safe.machkey, DESKEYLEN) != safe.machsum){
		warning("bad nvram key; using default password");
		/* Just so its not plain text in the binary */
		for(i=0; i<sizeof(defpass); i++)
			defpass[i] = (defpass[i]-19)^(17*(i+3));
		passtokey(safe.machkey, defpass);
	}
	safe.machsum = nvcsum(safe.machkey, DESKEYLEN);
	if(kflag){
		if(seek(fd, 1024+900, 0) < 0
		|| write(fd, &safe, sizeof safe) != sizeof safe)
			warning("can't write key to nvram");
	}
	close(fd);
	fd = open("#c/key", OWRITE);
	if(fd < 0)
		warning("can't open #c/key");
	else if(write(fd, safe.machkey, DESKEYLEN) != DESKEYLEN)
		warning("can't set #c/key");
	close(fd);
}
