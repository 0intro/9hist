#define uartwrreg(u,r,v)	outb((u)->port + (r), (u)->sticky[r] | (v))
#define uartrdreg(u,r)		inb((u)->port + (r))

#define uartpower(x, y)

void ns16552setup(ulong, ulong, char*, int);
void ns16552intr(int);

/*
 *  handle an interrupt to a single uart
 */
static void
ns16552intrx(Ureg*, void* arg)
{
	ns16552intr((ulong)arg);
}

void
ns16552install(void)
{
	static int already;

	if(already)
		return;
	already = 1;

	ns16552setup(Uart0, UartFREQ, "eia0", Ns550);
	intrenable(VectorUART0, ns16552intrx, (void*)0, BUSUNKNOWN);
	ns16552setup(Uart1, UartFREQ, "eia1", Ns550);
	intrenable(VectorUART1, ns16552intrx, (void*)0, BUSUNKNOWN);
}

#define RD(r)	inb(Uart0+(r))
static void
ns16552iputc(char c)
{
	mb();
	while((RD(5) & (1<<5)) == 0)
		mb();
	outb(Uart0, c);
	mb();
	while((RD(5) & (1<<5)) == 0)
		mb();
}

int
iprint(char *fmt, ...)
{
	int n, i, s;
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	n = doprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	s = splhi();
	for(i = 0; i < n; i++)
		ns16552iputc(buf[i]);
	splx(s);

	return n;
}
