#include <u.h>
#include <libc.h>
#include <../boot/boot.h>

int
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

void
getpasswd(char *p, int len)
{
	char c;
	int i, n, fd;

	fd = open("#c/consctl", OWRITE);
	if(fd < 0)
		fatal("can't open consctl; please reboot");
	write(fd, "rawon", 5);
 Prompt:		
	print("password: ");
	n = 0;
	for(;;){
		do{
			i = read(0, &c, 1);
			if(i < 0)
				fatal("can't read cons; please reboot");
		}while(i == 0);
		switch(c){
		case '\n':
			p[n] = '\0';
			close(fd);
			print("\n");
			return;
		case '\b':
			if(n > 0)
				n--;
			break;
		case 'u' - 'a' + 1:		/* cntrl-u */
			print("\n");
			goto Prompt;
		default:
			if(n < len - 1)
				p[n++] = c;
			break;
		}
	}
}
