#define outb(p, ch)			*(uchar*)((p)^7) = ch	
#define uartwrreg(u,r,v)	outb((u)->port + (r), (u)->sticky[r] | (v))
#define uartrdreg(u,r)		*(uchar*)(((u)->port + (r))^7)

#define uartpower(x, y)

void
ns16552install(void)
{
	static int already;

	if(already)
		return;
	already = 1;

	ns16552setup(Uart1, UartFREQ);
}

#define RD(r)	(*(uchar*)((Uart1+r)^7))
static void
ns16552iputc(char c)
{
	while((RD(5) & (1<<5)) == 0)
		;
	*(uchar*)(Uart1^7) = c;
	while((RD(5) & (1<<5)) == 0)
		;
}

int
iprint(char *fmt, ...)
{
	int n, i;
	char buf[512];

	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	for(i = 0; i < n; i++)
		ns16552iputc(buf[i]);
	
	return n;
}
