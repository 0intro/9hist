#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

void
key(int islocal, Method *mp)
{
	char password[20], key[7];
	int prompt, fd;

	USED(islocal);
	USED(mp);

	prompt = kflag;
	fd = open("#r/nvram", ORDWR);
	if(fd < 0){
		prompt = 1;
		warning("can't open nvram");
	}
	if(prompt){
		do
			getpasswd(password, sizeof password);
		while(!passtokey(key, password, strlen(password)));
	}else if(seek(fd, 1024+900, 0) < 0 || read(fd, key, 7) != 7){
		close(fd);
		warning("can't read key from nvram");
	}
	if(kflag && (seek(fd, 1024+900, 0) < 0 || write(fd, key, 7) != 7)){
		close(fd);
		warning("can't write key to nvram");
	}
	close(fd);
	fd = open("#c/key", OWRITE);
	if(fd < 0)
		warning("can't open key");
	else if(write(fd, key, 7) != 7)
		warning("can't write key");
	close(fd);
}

