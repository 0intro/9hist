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
	floppystart(0);
	for(;;){
		int c;

		c = getc(&kbdq);
		if(c!=-1)
			screenputc(c);
		idle();
		if((TK2SEC(m->ticks)%5)==0)
			if((TK2SEC(m->ticks)%10)==0)
				floppystop(0);
			else
				floppystart(0);
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
	spllo();
	for(;;)
		idle();
}

void
sched(void)
{
}
