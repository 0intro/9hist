/*
 *  PC specific code for the ns16552.  It includes support for the 2 built
 *  in uarts plus up to 5 MP-008 8 uart ISA cards.
 */
enum
{
	Maxcard= 5,		/* max serial cards */
	UartFREQ= 1843200,

	Serial=	0,
	Modem=	1,
};

#define uartwrreg(u,r,v)	outb((u)->port + r, (u)->sticky[r] | (v))
#define uartrdreg(u,r)		inb((u)->port + r)

void	ns16552setup(ulong, ulong, char*);

/*
 *  definition of an optional serial card
 */
typedef struct Scard
{
	ISAConf;	/* card configuration */
	int	first;	/* number of first port */
} Scard;
static Scard *scard[Maxcard]; 	/* configs for the serial card */

/* power management currently only makes sense on the AT&T safari */
static void
uartpower(int dev, int onoff)
{
	switch(dev){
	case Modem:
		if(modem(onoff) < 0)
			print("can't turn %s modem speaker\n", onoff?"on":"off");
		break;
	case Serial:
		if(serial(onoff) < 0)
			print("can't turn %s serial port power\n", onoff?"on":"off");
		break;
	}
}

/*
 *  handle an interrupt to a single uart
 */
static void
ns16552intrx(Ureg *ur, void *arg)
{
	USED(ur);

	ns16552intr((ulong)arg);
}

/*
 *  interrupts from the multiport card, MP-008.  A polling port
 *  tells which of 8 devices interrupted.
 */
static void
mp008intr(Ureg *ur, void *arg)
{
	int i, loops;
	uchar n;
	Scard *mp;

	USED(ur);
	mp = arg;
	for(loops = 0; loops < 1024; loops++){
		n = ~inb(mp->mem);
		if(n == 0)
			return;
		for(i = 0; n; i++){
			if(n & 1)
				ns16552intrx(ur, (void*)(mp->first+i));
			n >>= 1;
		}
	}
}

/*
 *  install the uarts (called by reset)
 */
void
ns16552install(void)
{
	int i, j, port, nscard;
	char *p;
	Scard *sc;
	char name[NAMELEN];
	static int already;

	if(already)
		return;
	already = 1;

	/* first two ports are always there and always the normal frequency */
	ns16552setup(0x3F8, UartFREQ, "eia0");
	setvec(Uart0vec, ns16552intrx, (void*)0);
	ns16552setup(0x2F8, UartFREQ, "eia1");
	setvec(Uart1vec, ns16552intrx, (void*)1);

	/* set up a serial console */
	p = getconf("console");
	if(p)
		ns16552special(atoi(p), 9600, &kbdq, &printq, kbdcr2nl);

	/* the rest come out of plan9.ini */
	nscard = 0;
	for(i = 0; i < Maxcard; i++){
		sc = scard[nscard] = xalloc(sizeof(Scard));
		if(isaconfig("serial", i, sc) == 0){
			xfree(sc);
			scard[nscard] = 0;
			continue;
		}

		if(strcmp(sc->type, "MP008") == 0 || strcmp(sc->type, "mp008") == 0){
			/*
			 *  port gives base port address for uarts
			 *  irq is interrupt
			 *  mem is the polling port
			 *  size is the number of serial ports on the same polling port
			 *  freq is the baud rate generator frequency
			 */
			if(sc->size == 0)
				sc->size = 8;
			if(sc->freq == 0)
				sc->freq = UartFREQ;
			sc->first = nuart;
			setvec(Int0vec+sc->irq, mp008intr, sc);
			port = sc->port;
			for(j=0; j < sc->size; j++){
				sprint(name, "eia%d%2.2d", nscard, j);
				ns16552setup(port, sc->freq, name);
				port += 8;
			}
		} else if(strcmp(sc->type, "com") == 0 || strcmp(sc->type, "COM") == 0){
			/*
			 *  port gives base port address for the uart
			 *  irq is interrupt
			 *  freq is the baud rate generator frequency
			 */
			if(sc->freq == 0)
				sc->freq = UartFREQ;
			sprint(name, "eia%d00", nscard);
			ns16552setup(sc->port, sc->freq, name);
			setvec(Int0vec+sc->irq, ns16552intrx, (void*)(nuart-1));
		}

		nscard++;
	}
}

int
iprint(char *fmt, ...)
{
	char buf[PRINTSIZE];
	int n;

	n = doprint(buf, buf+sizeof(buf), fmt, (&fmt+1)) - buf;
	screenputs(buf, n);

	return n;
}
