#include <u.h>
#include <libc.h>
#include <auth.h>
#include <../boot/boot.h>

char *homsg = "can't set user name or key; please reboot";

getsafe(char *field, int len, uchar *sum, char *file, int pass)
{
	char buf[64];

	if(nvcsum(field, len) != *sum){
		if(readfile(file, buf, sizeof(buf)) < 0){
			kflag |= 1;
			return -1;
		}
		memset(field, 0, len);
		if(pass)
			passtokey(field, buf);
		else
			strncpy(field, buf, len-1);
	}
	return 0;
}

void
key(int islocal, Method *mp)
{
	int fd, safeoff;
	Nvrsafe safe;
	char password[20];

	USED(islocal);
	USED(mp);

	if(strcmp(cputype, "sparc") == 0)
		safeoff = 1024+850;
	else
		safeoff = 1024+900;

	fd = open("#r/nvram", ORDWR);
	if(fd < 0
	|| seek(fd, safeoff, 0) < 0
	|| read(fd, &safe, sizeof safe) != sizeof safe){
		memset(&safe, 0, sizeof(safe));
		warning("can't read nvram");
	}
	if(getsafe(safe.machkey, DESKEYLEN, &safe.machsum, "#e/password", 1) < 0)
		warning("bad nvram key");
	if(getsafe(safe.authid, NAMELEN, &safe.authidsum, "#e/authid", 0) < 0)
		warning("bad authentication id");
	if(getsafe(safe.authdom, DOMLEN, &safe.authdomsum, "#e/authdom", 0) < 0)
		warning("bad authentication domain");
	if(kflag){
		do
			getpasswd(password, sizeof password);
		while(!passtokey(safe.machkey, password));
		outin("authid", safe.authid, sizeof(safe.authid));
		outin("authdom", safe.authdom, sizeof(safe.authdom));
		safe.machsum = nvcsum(safe.machkey, DESKEYLEN);
		safe.authidsum = nvcsum(safe.authid, sizeof(safe.authid));
		safe.authdomsum = nvcsum(safe.authdom, sizeof(safe.authdom));
		if(seek(fd, safeoff, 0) < 0
		|| write(fd, &safe, sizeof safe) != sizeof safe)
			warning("can't write key to nvram");
	}
	close(fd);

	/* set host's key */
	if(writefile("#c/key", safe.machkey, DESKEYLEN) < 0)
		fatal("#c/key");

	/* set host's owner (and uid of current process) */
	if(writefile("#c/hostowner", safe.authid, strlen(safe.authid)) < 0)
		fatal("#c/hostowner");

	/* set host's domain */
	if(writefile("#c/hostdomain", safe.authdom, strlen(safe.authdom)) < 0)
		fatal("#c/hostdomain");
}
