
/*
 *  Device Driver for National Semiconductor lm78
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"interp.h"
#include	<isa.h>
#include	"runt.h"

enum
{

/* Define the memory locations, registers,
 * and bit patterns for the nslm78 chip
 */

/* Define Address Register Port 0x05 */
	 ADDRESS_PORT		=	0x05,

/* Define Addresses of the various Registers in lm78 */
	CONFIG_REG		=	0x40,
	INTR_STATUS_REG1	=	0x41,
	INTR_STATUS_REG2	=	0x42,
	SMI_MASK_REG1		=	0x43,
	SMI_MASK_REG2		=	0x44,
	NMI_MASK_REG1		=	0x45,
	NMI_MASK_REG2		=	0x46,
	VID_FAN_REG		=	0x47,
	SERIAL_BUS_ADDR_REG	=	0x48,
	CHIP_RESET		=	0x49,
	VALUE_RAM_BASE_1	=	0x20,
	VALUE_RAM_BASE_2	=	0x60,
	VALUE_RAM_LIMIT_2B	=	0x6B,
	VALUE_RAM_LIMIT_2E	=	0x7D,
	ARRAY_SIZE	=	(VALUE_RAM_LIMIT_2E - VALUE_RAM_LIMIT_2B) + 1,

/* Define Data Register Port 0x06 */
	DATA_PORT		=	0x06,


/* Define bit patterns for Configuration Register */
	ONE_BIT			=	0x01,
	START_MONITOR		=	ONE_BIT,
	SMI_ENABLE		=	(ONE_BIT<<1),
	NMI_IRQ_ENABLE		=	(ONE_BIT<<2),
	INT_CLEAR		=	(ONE_BIT<<3),
	RESET			=	(ONE_BIT<<4),
	NMI_IRQ_SELECT		=	(ONE_BIT<<5),
	INITIALIZE		=	(ONE_BIT<<7),

	/* For now, we are not interested in handling the interrupts. */
	/* No bit patterns are defined for those interrupt registers */


	/* Do not plan to use Serial Bus Address */

/* Define Chip Reset/ID Register for lm78 */
	 CHIP_RESET_REG		=	0x49,

	/* Do not plan to use POST RAM (Power On Self Test) */

/* VALUE RAM Addresses for the Readings and Watchlog Limits */

		IN0_RD	=	0x60,
		IN1_RD	=	0x61,
		IN2_RD	=	0x62,
		IN3_RD	=	0x63,
		IN4_RD	=	0x64,
		IN5_RD	=	0x65,
		IN6_RD	=	0x66,
		TEMP_RD	=	0x67,
		FAN1_RD	=	0x68,
		FAN2_RD	=	0x69,
		FAN3_RD	=	0x6A,
		IN0_H	=	0x6B,
		IN0_L	=	0x6C,
		IN1_H	=	0x6D,
		IN1_L	=	0x6E,
		IN2_H	=	0x6F,
		IN2_L	=	0x70,
		IN3_H	=	0x71,
		IN3_L	=	0x72,
		IN4_H	=	0x73,
		IN4_L	=	0x74,
		IN5_H	=	0x75,
		IN5_L	=	0x76,
		IN6_H	=	0x77,
		IN6_L	=	0x78,
		TEMP_H	=	0x79,
		TEMP_L	=	0x7A,
		FAN1_CNT =	0x7B,
		FAN2_CNT =	0x7C,
		FAN3_CNT =	0x7D
};

enum
{
	Qdir,
	Qtemp,
	Qfan1,
	Qfan2,
	Qfan3,
	Qvolt1,
	Qvolt2,
	Qvolt3,
	Qvolt4,
	Qvolt5,
	Qvolt6,
	Qvolt7,
	Qalert,
};

Dirtab lm78tab[]={
	"temp",		{Qtemp, 0},	0,	0666,
	"fan1",		{Qfan1, 0},	0,	0666,
	"fan2",		{Qfan2, 0},	0,	0666,
	"fan3",		{Qfan3, 0},	0,	0666,
	"volt1",		{Qvolt1, 0},	0,	0666,
	"volt2",		{Qvolt2, 0},	0,	0666,
	"volt3",		{Qvolt3, 0},	0,	0666,
	"volt4",		{Qvolt4, 0},	0,	0666,
	"volt5",		{Qvolt5, 0},	0,	0666,
	"volt6",		{Qvolt6, 0},	0,	0666,
	"volt7",		{Qvolt7, 0},	0,	0666,
	"alert",	{Qalert, 0},	0,	0666,
};
#define Nlm78tab	nelem(lm78tab)
#define RAM_SIZE	30
#define	NFIELD		2
#define	NBUFSIZE	128
#define	N_DOT_PLACE	3

ulong	miBASE;			/* lm78 base address */

static int array_RAM[RAM_SIZE];
static int FanPulsesPerRev[] = { 0, 2, 2, 2 };
static int VoltageScale[] = 	{	0, 16, 16, 27, 
					60, 60,
					 16, 27
				};	/* per CPV5000 lm78 Access */
					/* volt1 for 3.3 v	   */
					/* volt2 for cpu v (2.7 v) */
					/* volt3 for 5.0 v	   */
					/* volt4 for 12  v	   */
					/* volt5 for -12 v	   */
static int default_val[] = {
				250, 187, 250, 125, 222, 148, 250, 167, 250, 167, 250,
				187, 222, 148,
				35, 40,
				219, 218, 217
			     };	/* voltages and fan speeds are converted to scales */
void
write_routine(int value, int ival);

static void
lm78reset(void)	/* Reset the lm78 Chip */
{
}

static void 
lm78detach(void)
{
}

static int
power(int x, int n) 
{
	int i, p;
	p = 1;
 	for (i = 1; i <= n; ++i)
		p = p * x;
	return p;
}

static void
write_ram (void)
{
	int i;

	outb(miBASE+ADDRESS_PORT, VALUE_RAM_LIMIT_2B);
	for (i=0; i < ARRAY_SIZE; i++) {
		outb(miBASE+DATA_PORT, default_val[i]);
		
	}
	return;
}
static void
set_intr_mask_regs(void)
{
	write_routine(0xe0, SMI_MASK_REG1); 
	write_routine(0x7f, SMI_MASK_REG2);	/* mask the interrupt status bits */
	write_routine(0xff, NMI_MASK_REG1);
	write_routine(0x7f, NMI_MASK_REG2);
	return;
}
static void
start_monitor(int conf_reg_content )
{
	
	conf_reg_content |= START_MONITOR;
	conf_reg_content &= ~INT_CLEAR ;
	conf_reg_content &= ~INITIALIZE ;

	outb( miBASE+ADDRESS_PORT,  CONFIG_REG);
	outb( miBASE+DATA_PORT, conf_reg_content);
	return;
}


static int
read_vid(void)
{
	int i,x;

	x = splhi();
	outb( miBASE+ADDRESS_PORT,  0x47); /* tell the chip, we want to 	*/
						/* read VID		*/
	i = inb(miBASE+DATA_PORT);
	splx(x);

	return i;	
}

enum
{
	IntelVendID=	0x8086,
	PiixID=		0x122E,
	Piix3ID=	0x7000,

	Piix4PMID=	0x7113,		/* PIIX4 power management function */

	PCSC=		0x78,		/* programmable chip select control register */
	PCSC8bytes=	0x01,
};

int
readpcilm78(void)
{	
	int pcs;
	Pcidev *p; 

	p = nil;
	while((p = pcimatch(p, IntelVendID, 0)) != nil){
		switch(p->did){
		/* these bridges have pretty easy access to the lm78 */
		case PiixID:
		case Piix3ID:
			pcs = pcicfgr16(p, PCSC);
			if(pcs & 3) {
				/* already enabled */
				miBASE = pcs & ~3;
				return 0;	
			}

			/* enable the chip, use default address 0x50 */
			pcicfgw16(p, PCSC, 0x50|PCSC8bytes);
			pcs = pcicfgr16(p, PCSC);
			miBASE = pcs & ~3;
			return 0;

		/* this bridge uses the SMbus */
		case Piix4PMID:
			return -1:
		}
	}
	return -1;
}

static void
lm78init(void)
{	
	int config_reg_content;

	if (readpcilm78() == -1) error("lm78 Not Initialized\n");

	outb( miBASE+ADDRESS_PORT, CONFIG_REG);
	config_reg_content = inb(miBASE+DATA_PORT);
	  /* Read in the config reg content  */

	config_reg_content |=  INITIALIZE; 	  /* After Power on the config_reg 		*/
						  /* has value 0000 1000			*/
						  /* For reset with all interrupt		*/
						  /* disabled, the config_reg is  		*/
						  /* to 1000 1000				*/

	outb( miBASE+ADDRESS_PORT, CONFIG_REG);
	outb( miBASE+DATA_PORT, config_reg_content);

	write_ram();	/* write to WATCHDOG RAM the default values */
	read_vid();
	set_intr_mask_regs();
	start_monitor(config_reg_content);
	
}

static Chan*
lm78attach(char *spec)
{
	
	return devattach('L', spec);
}

static Chan*
lm78clone(Chan* c, Chan* nc)
{
	return devclone(c, nc);
}

static int
lm78walk(Chan* c, char* name)
{
	return devwalk(c, name, lm78tab, Nlm78tab, devgen);
}

static void
lm78stat(Chan* c, char* db)
{	
	devstat(c, db, lm78tab, Nlm78tab, devgen);
}

static Chan*
lm78open(Chan* c, int omode)
{
	return devopen(c, omode, lm78tab, Nlm78tab, devgen);
}

static void
lm78create(Chan* c, char* name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

static void
lm78remove(Chan* c)
{
	USED(c);
	error(Eperm);
}

static void
lm78wstat(Chan* c, char* dp)
{
	USED(c, dp);
	error(Eperm);
}

static void
lm78close(Chan* c)
{
	USED(c);
}

static int
value_f(int f, int n_digit)
{
	int i, dvalue;
	dvalue = 0;
	for (i = n_digit; i >= 1; i--) {
		dvalue += (f%10) * power(10, N_DOT_PLACE-i);
		f /= 10;
	}
	return dvalue;
}

static int
aatof(char * str_ptr)
{
	int i, f;
	char *p;
	char *q;
	i = strtol(str_ptr, &p, 10);
	if (p == str_ptr) 
		error ("no digits\n");
		
	if (*p == 0) 
		return (i * 1000);
	f = strtol(p+1, &q,10);   /* do the part after the period */
	if (q > p+N_DOT_PLACE+1)  
		error("more than three digits after period\n");
	if (q == p+1)
		return (i*1000);
	
	if (q == p+2)
		return (i*1000 + value_f( f, 1));

	if (q == p+3)
		return (i*1000 + value_f( f, 2));

	if (q == p+4)
		return (i*1000 + value_f( f, 3));


}

void
reverse(char s[]) 
{
	int c, i, j;
	for (i = 0, j = strlen(s) -1; i < j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}


static void
itoreal(int n, char s[], int dot_place) 
{
	int i, sign;
	if ((sign =n) < 0)
		n = -n;
	i = 0;
	do {
		if (i != dot_place) {
			
			
			s[i++] = n % 10 + '0' ;
			n /= 10;
		} else
			s[i++] = '.';
	} while (n >0);
		
	
	if ( (sign > 0 ) && sign < power(10, dot_place)) {
		s[i++] = '.';
	};
	if ( (sign < 0 ) && (-sign < power(10, dot_place))) {
		s[i++] = '.';
		s[i++] = '-';
	};
	if (sign < 0)
		s[i++] = '-';

	s[i] = '\0';
	reverse(s);
}
	
static int
convert_to_temp(int val)
{	
	if (val == 0xff)
		return 0;
	if (val >= 0xc9)
		  return ( -((~val & 0177) + 1));

	return val;
	
}

static int
rpm_to_count(char* rpm, int index)
{	
	int n;
	n = 1350000/(FanPulsesPerRev[index] * aatof(rpm)/1000);
	if (n > 255)
		error(Ebadarg);
	return n;
}

static int
convert_to_rpm(int val, int index )
{
	
	if ((val==255) || (val == 0))
		return (0);
	else
		return(1350000/(FanPulsesPerRev[index] * val));
}

static char*
convert_to_volt(int val, int index)
{
	int i;
	char *vstring = malloc(12);
	if (vstring == 0) error("no memory");

	i = val * VoltageScale[index];
	itoreal(i, vstring, N_DOT_PLACE);
	return vstring;
}	

static int
voltage_to_scale( char *volt, int index)
{
	int n;	
	n = aatof(volt) / VoltageScale[index];
	if (n > 255) error(Ebadarg);
	else
	return n;
}

static int
read_routine(int ivalue)
{
	int x, iv;

	x = splhi();	/* turn off all maskable interrupts */

	outb(miBASE+ADDRESS_PORT, ivalue);
	iv = inb(miBASE+DATA_PORT );	
	
	splx(x);
	return iv;
}

static long
lm78read(Chan* c, void* a, long n, ulong offset)
{
	int value, lo, hi, fannum, v_num;
	int f_value, f_hi, f_lo, intr1, intr2;
	char *v_str, *v_hi, *v_lo;
	char buf[NBUFSIZE];

	USED(offset);

	v_str = nil;
	v_hi = nil;
	v_lo = nil;
	f_hi = 0;

	switch(c->qid.path & ~CHDIR){
	case Qdir:
		return devdirread(c,a,n,lm78tab, Nlm78tab,devgen);
	case Qtemp:
		/* read temperature from RAM and pass it back via *a */
		f_value = convert_to_temp(read_routine(TEMP_RD));
		f_hi = convert_to_temp(read_routine(TEMP_H));
		f_lo = convert_to_temp(read_routine(TEMP_L));
		
		break;
	case Qfan1:
		fannum = 1;
		value = read_routine(FAN1_RD);
		f_value = convert_to_rpm(value, fannum);
		lo = read_routine(FAN1_CNT);
		f_lo = convert_to_rpm(lo, fannum);
		break;
	case Qfan2:
		fannum = 2;
		value = read_routine(FAN2_RD);
		f_value = convert_to_rpm(value, fannum);
		lo = read_routine(FAN2_CNT);
		f_lo = convert_to_rpm(lo, fannum);
		break;
	case Qfan3:
		fannum = 3;
		value = read_routine(FAN3_RD);
		f_value = convert_to_rpm(value, fannum);
		lo = read_routine(FAN3_CNT);
		f_lo = convert_to_rpm(lo, fannum);
		break;

	default:
		switch(c->qid.path & ~CHDIR){
		
		case Qvolt1:
			v_num = 1;
			value = read_routine(IN0_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN0_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN0_L);
			v_lo = convert_to_volt(lo, v_num);

			
			break;
		case Qvolt2:
			v_num = 2;
			value = read_routine(IN1_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN1_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN1_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		case Qvolt3:
			v_num = 3;
			value = read_routine(IN2_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN2_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN2_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		case Qvolt4:
			v_num = 4;
			value = read_routine(IN3_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN3_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN3_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		case Qvolt5:
			v_num = 5;
			value = read_routine(IN4_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN4_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN4_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		case Qvolt6:
			v_num =6;
			value = read_routine(IN5_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN5_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN5_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		case Qvolt7:
			v_num = 7;
			value = read_routine(IN6_RD);
			v_str = convert_to_volt(value, v_num);
			hi = read_routine(IN6_H);
			v_hi = convert_to_volt(hi, v_num);
			lo = read_routine(IN6_L);
			v_lo = convert_to_volt(lo, v_num);
			break;
		
		case Qalert:
			/* read interrupt status registers */
			/* delay(15*100);		   */

			
			intr1 = read_routine(INTR_STATUS_REG1);
			intr2 = read_routine(INTR_STATUS_REG2);
			intr1 &= 0x001F;
			intr2 &= 0x001F;
			n = sprint(buf, "%d", intr1);
			return readstr(offset, a, n, buf); /* deliver data to address a */

		default:
			error(Ebadarg);
			
		}

		/* get ready to send to kernel */

	
		sprint(buf, "%s %s %s", v_str, v_lo, v_hi);
		free(v_str);
		free(v_hi);
		free(v_lo);
		return readstr(offset, a, n, buf);
	}


	n = sprint(buf, "%d %d %d", f_value, f_lo, f_hi);
	return readstr(offset, a, n, buf); /* deliver data to address a */
}

void
write_routine(int value, int ival)
{
	int	x;

	x = splhi();	/* turn off all maskable interrupts	  */	
			/* take the input from *a convert to int  */
			/* and write it to the RAM		  */
	outb( miBASE+ADDRESS_PORT, ival);
	outb( miBASE+DATA_PORT, value);
	splx(x);
	return;
}

int
tempvalue( char *ptr)
{	
	int temp_digit;
	char *p;

	temp_digit = strtol(ptr, &p, 10);
	if (p == ptr) 
		error(Ebadarg);
	if (temp_digit > 125)
		error(Ebadarg);
	return temp_digit;
}
static Block*
lm78bread(Chan* c, long n, ulong offset)
{
	return devbread(c, n, offset);
}

static long
lm78write(Chan* c, char* a, long n, ulong offset)
{	
	int nf, fan_num, v_num;
	char *field[NFIELD], buf[NBUFSIZE];

	USED(offset);
	if(n > sizeof(buf)-1)
		n = sizeof(buf)-1;

	memmove(buf, a, n);
	buf[n] = '\0';
	nf = parsefields(buf, field, NFIELD, " \t");

	USED(nf);
	switch(c->qid.path & ~CHDIR){
	case Qtemp:
		write_routine(tempvalue(field[1]), TEMP_H);
		delay(1000);
		write_routine(tempvalue(field[0]), TEMP_L);  
		break;
	case Qfan1:
		fan_num = 1;
		write_routine(rpm_to_count(field[0], fan_num), FAN1_CNT);
		break;
	case Qfan2:
		fan_num = 2;
		write_routine(rpm_to_count(field[0], fan_num), FAN2_CNT); 
		break;
	case Qfan3:
		fan_num = 3;
		write_routine(rpm_to_count(field[0], fan_num), FAN3_CNT); 
		break;
	case Qvolt1:
		v_num = 1;
		write_routine(voltage_to_scale(field[1], v_num), IN0_H);
		write_routine(voltage_to_scale(field[0], v_num), IN0_L); 	
		break;
	case Qvolt2:
		v_num = 2;
		write_routine(voltage_to_scale(field[1], v_num), IN1_H);
		write_routine(voltage_to_scale(field[0], v_num), IN1_L);
		break;
	case Qvolt3:
		v_num = 3;
		write_routine(voltage_to_scale(field[1], v_num), IN2_H);
		write_routine(voltage_to_scale(field[0], v_num), IN2_L);
		break;
	case Qvolt4:
		v_num = 4;
		write_routine(voltage_to_scale(field[1], v_num), IN3_H);
		write_routine(voltage_to_scale(field[0], v_num), IN3_L);	
		break;
	case Qvolt5:
		v_num = 5;
		write_routine(voltage_to_scale(field[1], v_num), IN4_H);
		write_routine(voltage_to_scale(field[0], v_num), IN4_L);
		break;
	case Qvolt6:
		v_num = 6;
		write_routine(voltage_to_scale(field[1], v_num), IN5_H);
		write_routine(voltage_to_scale(field[0], v_num), IN5_L);
		break;
	case Qvolt7:
		v_num = 7;
		write_routine(voltage_to_scale(field[1], v_num), IN6_H);
		write_routine(voltage_to_scale(field[0], v_num), IN6_L);
		break;
	default:
		error(Ebadarg);
	}
	return n;
}

static long
lm78bwrite(Chan* c, Block* bp, ulong offset)
{
	return devbwrite(c, bp, offset);
}

Dev lm78devtab = {
	'L',
	"lm78",
	lm78reset,
	lm78init,
	lm78attach,
	lm78detach,
	lm78clone,
	lm78walk,
	lm78stat,
	lm78open,
	lm78create,
	lm78close,
	lm78read,
	lm78bread,
	lm78write,
	lm78bwrite,
	lm78remove,
	lm78wstat,
};
