#include <u.h>
#include <libc.h>
#include <auth.h>
#include <../boot/boot.h>

static uchar
cksum(char *key)
{
	int i, nvsum;

	nvsum = 0;
	for(i=0; i<DESKEYLEN; i++)
		nvsum += key[i];
	return nvsum & 0xff;
}

void
key(int islocal, Method *mp)
{
	char password[20], key[DESKEYLEN];
	uchar nvsum;
	int prompt, fd;

	USED(islocal);
	USED(mp);

	prompt = kflag;
	fd = open("#r/nvram", ORDWR);
	if(fd < 0){
		prompt = 1;
		warning("can't open nvram");
	}
	if(seek(fd, 1024+900, 0) < 0
	|| read(fd, key, DESKEYLEN) != DESKEYLEN
	|| read(fd, &nvsum, 1) != 1)
		warning("can't read nvram key");

	if(prompt){
		do
			getpasswd(password, sizeof password);
		while(!passtokey(key, password));
	}else if(cksum(key) != nvsum){
		warning("bad nvram key; using password boofhead");
		passtokey(key, "boofhead");
	}
	nvsum = cksum(key);
	if(kflag){
		if(seek(fd, 1024+900, 0) < 0
		|| write(fd, key, DESKEYLEN) != DESKEYLEN
		|| write(fd, &nvsum, 1) != 1)
			warning("can't write key to nvram");
	}
	close(fd);
	fd = open("#c/key", OWRITE);
	if(fd < 0)
		warning("can't open #c/key");
	else if(write(fd, key, DESKEYLEN) != DESKEYLEN)
		warning("can't set #c/key");
	close(fd);
}
