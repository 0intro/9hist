#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"

main(void)
{
	screeninit();
	print("screen inited\n");
	trapinit();
	print("traps inited\n");
	kbdinit();
	print("kbd inited\n");
	sti();
	for(;;);
}

void
delay(int l)
{
	int i;

	while(--l){
		for(i=0; i < 10000; i++)
			;
	}
}

int
sprint(char *s, char *fmt, ...)
{
	return doprint(s, s+PRINTSIZE, fmt, (&fmt+1)) - s;
}

int
print(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	screenputs(buf, n);
	return n;
}

void
panic(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	screenputs("panic: ", 7);
	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	screenputs(buf, n);
	for(;;);
}
