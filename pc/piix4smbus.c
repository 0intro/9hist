#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

//
//	SMBus support for the PIIX4
//
enum
{
	IntelVendID=	0x8086,
	Piix4PMID=	0x7113,		/* PIIX4 power management function */

	// SMBus configuration registers (function 3)
	SMBbase=	0x90,		// 4 byte base address (bit 0 == 1, bit 3:1 == 0)
	SMBconfig=	0xd2,
	 SMBintrselect=	(7<<1),
	  SMIenable=	(0<<1),		//  interrupts sent to SMI#
	  IRQ9enable=	(4<<1),		//  intettupts sent to IRQ9
	 SMBenable=	(1<<0),		//  1 enables

	// SMBus IO space registers
	Hoststatus=	0x0,
	 Failed=	(1<<4),	 	//  transaction terminated by KILL (reset by writing 1)
	 Bus_error=	(1<<3),		//  transactio collision (reset by writing 1)
	 Dev_error=	(1<<2),		//  device error interrupt (reset by writing 1)
	 Host_complete=	(1<<2),		//  host command completion interrupt (reset by writing 1)
	 Host_busy=	(1<<0),		//
	Slavestatus=	0x1,
	 Alert_sts=	(1<<5),		//  someone asserted SMBALERT# (reset by writing 1)
	 Shdw2_sts=	(1<<4),		//  slave accessed shadow 2 port (reset by writing 1)
	 Shdw1_sts=	(1<<3),		//  slave accessed shadow 1 port (reset by writing 1)
	 Slv_sts=	(1<<2),		//  slave accessed shadow 1 port (reset by writing 1)
	 Slv_bsy=	(1<<0),
	Hostcontrol=	0x2,
	 Start=		(1<<6),		//  start execution
	 Cmd_prot=	(7<<2),		//  command protocol mask
	  Quick=	(0<<2),		//   address only
	  Byte=		(1<<2),		//   address + cmd
	  ByteData=	(2<<2),		//   address + cmd + data
	  WordData=	(3<<2),		//   address + cmd + data + data
	 Kill=		(1<<1),		//  abort in progress command
	 Ienable=	(1<<0),		//  enable completion interrupts
	Hostcommand=	0x3,
	Hostaddress=	0x4,
	 AddressMask=	(0x7f<<1),	//  target address
	 RW=		(1<<0),		//  1 == read, 0 == write
	Hostdata0=	0x5,
	Hostdata1=	0x6,
	Blockdata=	0x7,
	Slavecontrol=	0x8,
	 Alert_en=	(1<<3),		//  enable interrupt on SMBALERT#
	 Shdw2_en=	(1<<2),		//  enable interrupt on external shadow 2 access
	 Shdw1_en=	(1<<1),		//  enable interrupt on external shadow 1 access
	 Slv_en=	(1<<0),		//  enable interrupt on access of host controller slave port
	Shadowcommand=	0x9,
	Slaveevent=	0xa,
	Slavedata=	0xc,
};

static int
quickcommand(SMBus *s, int addr)
{
}

static int
send(SMBus *s, int addr, int data)
{
}

static int
recv(SMBus *s, int addr. int *data)
{
}

static int
bytewrite(SMBus *s, int addr, int cmd, int data)
{
}

static int
byteread(SMBus *s, int addr, int cmd. int *data)
{
}

static int
wordwrite(SMBus *s, int addr, int cmd, int data)
{
}

static int
wordread(SMBus *s, int addr, int cmd. int *data)
{
}

static SMBus proto =
{
	.quick = quick,
	.send = send,
	.recv = recv,
	.bytewrite = bytewrite,
	.byteread = byteread,
	.wordwrite = wordwrite,
	.wordread = wordread,
};

//
//  return 0 if this is a piix4 with an smbus interface
//
SMBus*
piix4smbus(void)
{
	int pcs;
	Pcidev *p;
	static SMBus *s;

	if(s != nil)
		return s;

	p = pcimatch(p, IntelVendID, Piix4PMID));
	if(p == nil)
		return nil;

	s = smalloc(sizeof(*s));	
	memmove(s, &proto, sizeof(*s));
	s->arg = p;

	pcicfgw8(p->tbdf, SMBconfig, IRQ9enable|0);		// disable the smbus
	pcicfgw8(p->tbdf, SMBbase, 0x50);			// default address
	pcicfgw8(p->tbdf, SMBconfig, IRQ9enable|SMBenable);	// enable the smbus

	return s;
}
