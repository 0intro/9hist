#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

main(void)
{
	screeninit();
	trapinit();
	kbdinit();
	clockinit();
	mmuinit();
	floppyinit();
	spllo();
	for(;;){
		int c;

		c = getc(&kbdq);
		if(c!=-1)
			screenputc(c);
		idle();
	}
}

void
delay(int l)
{
	int i;

	while(--l){
		for(i=0; i < 100; i++)
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
	screenputs("\n", 1);
	INT0ENABLE;
	spllo();
	for(;;)
		idle();
}

void
sched(void)
{
}
