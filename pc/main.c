#include <u.h>

#define	WIDTH	80
#define	HEIGHT	22
#define SCREEN	((char *)0xB8000)
int pos;

void
prchar(int x)
{
	if(x == '\n'){
		pos = pos/WIDTH;
		pos = (pos+1)*WIDTH;
	} else {
		SCREEN[pos++] = x;
		SCREEN[pos++] = 0x43;
	}
	if(pos >= WIDTH*HEIGHT)
		pos = 0;
}

void
prstr(char *s)
{
	while(*s)
		prchar(*s++);
}

void
prdig(ulong x)
{
	if(x < 0xA)
		prchar('0' + x);
	else
		prchar('A' + x);
}

void
prhex(ulong x)
{
	ulong y;

	if(x <= 0xF){
		prdig(x);
	} else {
		y = x&0xF;
		prhex(x>>4);
		prdig(y);
	}
}

main(void)
{
	prstr("hello world\n");
}

