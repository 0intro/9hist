#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"init.h"
#include	"pool.h"

Mach *m;
Conf conf;

static void	gpioinit(void);

void
main(void)
{
	/* zero out bss */
	memset(edata, 0, end-edata);

	/* point to Mach structure */
	m = (Mach*)MACHADDR;

	rs232power(1);
	iprint("bitsy kernel\n");
	confinit();
	iprint("%d pages %lux(%lud) %lux(%lud)\n", conf.npage, conf.base0, conf.npage0, conf.base1, conf.npage1);
	xinit();
	mmuinit();
	sa1100_uartsetup();
	gpioinit();
	trapinit();
	clockinit();
	spllo();
	delay(1000);
	exit(1);
}

/*
 *  exit kernel either on a panic or user request
 */
void
exit(int ispanic)
{
	void (*f)();

	USED(ispanic);
	delay(1000);

	iprint("it's a wonderful day to die\n");
	mmudisable();
	f = nil;
	(*f)();
}

/*
 *  set mach dependent process state for a new process
 */
void
procsetup(Proc *p)
{
	p->fpstate = FPinit;
}

/*
 *  Save the mach dependent part of the process state.
 */
void
procsave(Proc *p)
{
	USED(p);
}

/* place holder */
/*
 *  dummy since rdb is not included 
 */
void
rdb(void)
{
}

/*
 *  probe the last location in a meg of memory, make sure it's not
 *  reflected into something else we've already found.
 */
int
probemem(ulong addr)
{
	ulong *p;
	ulong a;

	addr += OneMeg - sizeof(ulong);
	p = (ulong*)addr;
	*p = addr;
	for(a = conf.base0+OneMeg-sizeof(ulong); a < conf.npage0; a += OneMeg){
		p = (ulong*)a;
		*p = 0;
	}
	for(a = conf.base1+OneMeg-sizeof(ulong); a < conf.npage1; a += OneMeg){
		p = (ulong*)a;
		*p = 0;
	}
	p = (ulong*)addr;
	if(*p != addr)
		return -1;
	return 0;
}

/*
 *  we assume that the kernel is at the beginning of one of the
 *  contiguous chunks of memory.
 */
void
confinit(void)
{
	int i;
	ulong addr;
	ulong ktop;

	/* find first two contiguous sections of available memory */
	addr = PHYSDRAM0;
	conf.base0 = conf.npage0 = addr;
	conf.base1 = conf.npage1 = addr;
	for(i = 0; i < 512; i++){
		if(probemem(addr) == 0)
			break;
		addr += OneMeg;
	}
	for(; i < 512; i++){
		if(probemem(addr) < 0)
			break;
		addr += OneMeg;
		conf.npage0 = addr;
	}

	conf.base1 = conf.npage1 = addr;
	for(; i < 512; i++){
		if(probemem(addr) == 0)
			break;
		addr += OneMeg;
	}
	for(; i < 512; i++){
		if(probemem(addr) < 0)
			break;
		addr += OneMeg;
		conf.npage1 = addr;
	}

	/* take kernel out of allocatable space */
	ktop = PGROUND((ulong)end);
	if(ktop >= conf.base0 && ktop <= conf.npage0)
		conf.base0 = ktop;
	else if(ktop >= conf.base1 && ktop <= conf.npage1)
		conf.base1 = ktop;
	else
		iprint("kernel not in allocatable space\n");

	/* make npage the right thing */
	conf.npage0 = (conf.npage0 - conf.base0)/BY2PG;
	conf.npage1 = (conf.npage1 - conf.base1)/BY2PG;
	conf.npage = conf.npage0+conf.npage1;

	if(conf.npage > 16*MB/BY2PG){
		conf.upages = (conf.npage*60)/100;
		imagmem->minarena = 4*1024*1024;
	}else
		conf.upages = (conf.npage*40)/100;
	conf.ialloc = ((conf.npage-conf.upages)/2)*BY2PG;

	conf.nmach = 1;

	/* set up other configuration parameters */
	conf.nproc = 100;
	conf.nswap = conf.npage*3;
	conf.nswppo = 4096;
	conf.nimage = 200;

	conf.monitor = 1;

	conf.copymode = 0;		/* copy on write */
}

GPIOregs *gpioregs;
ulong *egpioreg;

static void
gpioinit(void)
{
	gpioregs = mapspecial(GPIOREGS, 32);
	gpioregs->direction = 
		GPIO_LDD8_o|GPIO_LDD9_o|GPIO_LDD10_o|GPIO_LDD11_o
		|GPIO_LDD12_o|GPIO_LDD13_o|GPIO_LDD14_o|GPIO_LDD15_o
		|GPIO_CLK_SET0_o|GPIO_CLK_SET1_o
		|GPIO_L3_SDA_io|GPIO_L3_MODE_o|GPIO_L3_SCLK_o
		|GPIO_COM_RTS_o;
	gpioregs->rising = 0;
	gpioregs->falling = 0;

	egpioreg = mapspecial(EGPIOREGS, 4);
}

static ulong egpiosticky;

void
rs232power(int on)
{
	if(on)
		egpiosticky |= EGPIO_rs232_power;
	else
		egpiosticky &= ~EGPIO_rs232_power;
	*egpioreg = egpiosticky;
}

void
irpower(int on)
{
	if(on)
		egpiosticky |= EGPIO_ir_power;
	else
		egpiosticky &= ~EGPIO_ir_power;
	*egpioreg = egpiosticky;
}

void
lcdpower(int on)
{
	if(on)
		egpiosticky |= EGPIO_lcd_3v|EGPIO_lcd_ic_power|EGPIO_lcd_5v|EGPIO_lcd_9v;
	else
		egpiosticky &= ~(EGPIO_lcd_3v|EGPIO_lcd_ic_power|EGPIO_lcd_5v|EGPIO_lcd_9v);
	*egpioreg = egpiosticky;
}
