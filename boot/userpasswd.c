#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

char	password[NAMELEN];

static int
passtokey(char *key, char *p, int n)
{
	uchar t[10];
	int c;

	memset(t, ' ', sizeof t);
	if(n < 5)
		return 0;
	if(n > 10)
		n = 10;
	strncpy((char*)t, p, n);
	if(n >= 9){
		c = p[8] & 0xf;
		if(n == 10)
			c += p[9] << 4;
		for(n = 0; n < 8; n++)
			if(c & (1 << n))
				t[n] -= ' ';
	}
	for(n = 0; n < 7; n++)
		key[n] = (t[n] >> n) + (t[n+1] << (8 - (n+1)));
	return 1;
}

/*
 *  get/set user name and password.  verify password with auth server.
 */
void
userpasswd(Method *mp)
{
	char key[7];
	char buf[8 + NAMELEN];
	int fd, crfd;

	if(*username == 0 || strcmp(username, "none") == 0){
		strcpy(username, "none");
		outin("user", username, sizeof(username));
	}
	crfd = fd = -1;
	while(strcmp(username, "none") != 0 && strcmp(mp->name, "local") != 0){
		getpasswd(password, sizeof password);
		if(!passtokey(key, password, strlen(password))){
			print("bad password; try again\n");
			continue;
		}
		fd = open("#c/key", OWRITE);
		if(fd < 0)
			fatal("can't open #c/key; please reboot");
		if(write(fd, key, 7) != 7)
			fatal("can't write #c/key; please reboot");
		close(fd);
		crfd = open("#c/crypt", ORDWR);
		if(crfd < 0)
			fatal("can't open crypt file");
		write(crfd, "E", 1);
		fd = (*mp->auth)();
		if(fd < 0){
			warning("password not checked!");
			break;
		}
		strncpy(buf+8, username, NAMELEN);
		if(read(fd, buf, 8) != 8
		|| write(crfd, buf, 8) != 8
		|| read(crfd, buf, 8) != 8
		|| write(fd, buf, 8 + NAMELEN) != 8 + NAMELEN){
			warning("password not checked!");
			break;
		}
		if(read(fd, buf, 2) == 2 && buf[0]=='O' && buf[1]=='K')
			break;
		close(fd);
		outin("user", username, sizeof(username));
	}
	close(fd);
	close(crfd);

	/* set user now that we're sure */
	fd = open("#c/user", OWRITE|OTRUNC);
	if(fd >= 0){
		if(write(fd, username, strlen(username)) < 0)
			warning("write user name");
		close(fd);
	}else
		warning("open #c/user");
}
