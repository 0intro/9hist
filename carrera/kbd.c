#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"

enum
{
	/*
	 *  commands
	 */
	Krdcmd=		0x20,	/* read command byte */
	Kwrcmd=		0x60,	/* write command byte */
	Kselftest=	0xAA,	/* self test */
	Ktest=		0xAB,	/* keyboard test */
	Kdisable=	0xAD,	/* disable keyboard */
	Kenable=	0xAE,	/* enable keyboard */
	Kmseena=	0xA8,	/* enable mouse */
	Krdin=		0xC0,	/* read input port */
	Krdout=		0xD0,	/* read output port */
	Kwrout=		0xD1,	/* write output port */
	Krdtest=	0xE0,	/* read test inputs */
	Kwrlights=	0xED,	/* set lights */
	Kreset=		0xF0,	/* soft reset */
	/*
	 *  magic characters
	 */
	Msetscan=	0xF0,	/* set scan type (0 == unix) */
	Menable=	0xF4,	/* enable the keyboard */
	Mdisable=	0xF5,	/* disable the keyboard */
	Mdefault=	0xF6,	/* set defaults */
	Mreset=		0xFF,	/* reset the keyboard */
	/*
	 *  responses from keyboard
	 */
	Rok=		0xAA,		/* self test OK */
	Recho=		0xEE,		/* ??? */
	Rack=		0xFA,		/* command acknowledged */
	Rfail=		0xFC,		/* self test failed */
	Rresend=	0xFE,		/* ??? */
	Rovfl=		0xFF,		/* input overflow */
	/*
	 *  command register bits
	 */
	Cintena=	1<<0,	/* enable output interrupt */
	Cmseint=	1<<1,	/* enable mouse interrupt */
	Csystem=	1<<2,	/* set system */
	Cinhibit=	1<<3,	/* inhibit override */
	Cdisable=	1<<4,	/* disable keyboard */
	/*
	 *  output port bits
	 */
	Osoft=		1<<0,	/* soft reset bit (must be 1?) */
	Oparity=	1<<1,	/* force bad parity */
	Omemtype=	1<<2,	/* simm type (1 = 4Mb, 0 = 1Mb)	*/
	Obigendian=	1<<3,	/* big endian */
	Ointena=	1<<4,	/* enable interrupt */
	Oclear=		1<<5,	/* clear expansion slot interrupt */
	/*
	 *  status bits
	 */
	Sobf=		1<<0,	/* output buffer full */
	Sibf=		1<<1,	/* input buffer full */
	Ssys=		1<<2,	/* set by self-test */
	Slast=		1<<3,	/* last access was to data */
	Senabled=	1<<4,	/* keyboard is enabled */
	Stxtimeout=	1<<5,	/* transmit to kybd has timed out */
	Srxtimeout=	1<<6,	/* receive from kybd has timed out */
	Sparity=	1<<7,	/* parity on byte was even */
	/*
	 *  light bits
	 */
	L1=		1<<0,	/* light 1, network activity */
	L2=		1<<2,	/* light 2, caps lock */
	L3=		1<<1,	/* light 3, no label */
};

#define KBDCTL	(*(uchar*)(KeyboardIO+Keyctl))
#define KBDDAT	(*(uchar*)(KeyboardIO+Keydat))
#define OUTWAIT	while(KBDCTL & Sibf); kdbdly(1)
#define INWAIT	while(!(KBDCTL & Sobf)); kdbdly(1)
#define ACKWAIT INWAIT ; if(KBDDAT != Rack) print("bad response\n"); kdbdly(1)

enum
{
	Spec=	0x80,

	PF=	Spec|0x20,	/* num pad function key */
	View=	Spec|0x00,	/* view (shift window up) */
	F=	Spec|0x40,	/* function key */
	Shift=	Spec|0x60,
	Break=	Spec|0x61,
	Ctrl=	Spec|0x62,
	Latin=	Spec|0x63,
	Up=	Spec|0x70,	/* key has come up */
	No=	Spec|0x7F,	/* no mapping */

	Tmask=	Spec|0x60,
};

uchar keymap[] = {
[0]	No,	No,	No,	No,	No,	No,	No,	F|1,
	'\033',	No,	No,	No,	No,	'\t',	'`',	F|2,
[0x10]	No,	Ctrl,	Shift,	Shift,	Shift,	'q',	'1',	F|3,
	No,	Shift,	'z',	's',	'a',	'w',	'2',	F|4,
[0x20]	No,	'c',	'x',	'd',	'e',	'4',	'3',	F|5,
	No,	' ',	'v',	'f',	't',	'r',	'5',	F|6,
[0x30]	No,	'n',	'b',	'h',	'g',	'y',	'6',	F|7,
	No,	View,	'm',	'j',	'u',	'7',	'8',	F|8,
[0x40]	No,	',',	'k',	'i',	'o',	'0',	'9',	F|9,
	No,	'.',	'/',	'l',	';',	'p',	'-',	F|10,
[0x50]	No,	No,	'\'',	No,	'[',	'=',	F|11,	'\r',
	Latin,	Shift,	'\n',	']',	'\\',	No,	F|12,	Break,
[0x60]	View,	View,	Break,	Shift,	'\177',	No,	'\b',	No,
	No,	'1',	View,	'4',	'7',	',',	No,	No,
[0x70]	'0',	'.',	'2',	'5',	'6',	'8',	PF|1,	PF|2,
	No,	'\n',	'3',	No,	PF|4,	'9',	PF|3,	No,
[0x80]	No,	No,	No,	No,	'-',	No,	No,	No,
};

uchar skeymap[] = {
[0]	No,	No,	No,	No,	No,	No,	No,	F|1,
	'\033',	No,	No,	No,	No,	'\t',	'~',	F|2,
[0x10]	No,	Ctrl,	Shift,	Shift,	Shift,	'Q',	'!',	F|3,
	No,	Shift,	'Z',	'S',	'A',	'W',	'@',	F|4,
[0x20]	No,	'C',	'X',	'D',	'E',	'$',	'#',	F|5,
	No,	' ',	'V',	'F',	'T',	'R',	'%',	F|6,
[0x30]	No,	'N',	'B',	'H',	'G',	'Y',	'^',	F|7,
	No,	View,	'M',	'J',	'U',	'&',	'*',	F|8,
[0x40]	No,	'<',	'K',	'I',	'O',	')',	'(',	F|9,
	No,	'>',	'?',	'L',	':',	'P',	'_',	F|10,
[0x50]	No,	No,	'"',	No,	'{',	'+',	F|11,	'\r',
	Latin,	Shift,	'\n',	'}',	'|',	No,	F|12,	Break,
[0x60]	View,	View,	Break,	Shift,	'\177',	No,	'\b',	No,
	No,	'1',	View,	'4',	'7',	',',	No,	No,
[0x70]	'0',	'.',	'2',	'5',	'6',	'8',	PF|1,	PF|2,
	No,	'\n',	'3',	No,	PF|4,	'9',	PF|3,	No,
[0x80]	No,	No,	No,	No,	'-',	No,	No,	No,
};


struct Kbd
{
	Lock;
	int l;
} kbd;

void
kdbdly(int l)
{
	int i;

	l *= 21;	/* experimentally determined */
	for(i=0; i<l; i++)
		;
}

/*
 *  wait for a keyboard event (or some max time)
 */
int
kbdwait(void)
{
	int tries;

	for(tries = 0; tries < 2000; tries++){
		if(KBDCTL & Sobf)
			return 1;
		kdbdly(1);
	}
	return 0;
}

/*
 *  wait for a keyboard acknowledge (or some max time)
 */
int
kbdackwait(void)
{
	if(kbdintr())
		return KBDDAT;
	return 0;
}

void
mouseintr(void)
{
	uchar c;
	static int nb;
	int buttons, dx, dy;
	static short msg[3];
	static uchar b[] = {0, 1, 4, 5, 2, 3, 6, 7, 0, 1, 2, 5, 2, 3, 6, 7 };

	kbdwait();
	c = KBDDAT;

	/* 
	 *  check byte 0 for consistency
	 */
	if(nb==0 && (c&0xc8)!=0x08)
		return;

	msg[nb] = c;
	if(++nb == 3){
		nb = 0;
		if(msg[0] & 0x10)
			msg[1] |= 0xFF00;
		if(msg[0] & 0x20)
			msg[2] |= 0xFF00;

		buttons = b[msg[0]&7];
		dx = msg[1];
		dy = -msg[2];
		mousetrack(buttons, dx, dy);
	}
}

int
kbdintr(void)
{
	int c, i, nk;
	uchar ch, code;
	static uchar kc[5];
	static int shifted, ctrled, lstate;
	static int upcode;

	kbdwait();
	code = KBDDAT;

	/*
	 *  key has gone up
	 */
	if(code == Up) {
		upcode = 1;
		return 0;
	}

	if(code > 0x87)
		return 1;

	if(upcode){
		ch = keymap[code];
		if(ch == Ctrl)
			ctrled = 0;
		else if(ch == Shift)
			shifted = 0;
		upcode = 0;
		return 0;
	}
	upcode = 0;

	/*
	 *  convert
	 */
	if(shifted)
		ch = skeymap[code];
	else
		ch = keymap[code];
	/*
 	 *  normal character
	 */
	if(!(ch & Spec)){
		if(ctrled)
			ch &= 0x1f;
		switch(lstate){
		case 1:
			kc[0] = ch;
			lstate = 2;
			if(ch == 'X')
				lstate = 3;
			break;
		case 2:
			kc[1] = ch;
			c = latin1(kc);
			nk = 2;
		putit:
			lstate = 0;
			if(c != -1)
				kbdputc(kbdq, c);
			else {
				for(i=0; i<nk; i++)
					kbdputc(kbdq, kc[i]);
			}
			break;
		case 3:
		case 4:
		case 5:
			kc[lstate-2] = ch;
			lstate++;
			break;
		case 6:
			kc[4] = ch;
			c = unicode(kc);
			nk = 5;
			goto putit;
		default:
			kbdputc(kbdq, ch);
			break;
		}
		return 0;
	}

	/*
	 *  filter out function keys
	 */
	if((Tmask&ch) == (Spec|F))
		return 0;

	/*
	 *  special character
	 */
	switch(ch){
	case Shift:
		shifted = 1;
		break;
	case Break:
		break;
	case Ctrl:
		ctrled = 1;
		break;
	case Latin:
		lstate = 1;
		break;
	default:
		kbdputc(kbdq, ch);
	}
	return 0;
}

void
lights(int l)
{
	int s;
	int tries;

	s = splhi();
	for(tries = 0; tries < 2000 && (KBDCTL & Sibf); tries++)
		;
	kdbdly(1);
	KBDDAT = Kwrlights;
	kbdackwait();
	for(tries = 0; tries < 2000 && (KBDCTL & Sibf); tries++)
		;
	kdbdly(1);
	KBDDAT = kbd.l = l;
	kbdackwait();
	splx(s);
}

static void
empty(void)
{
	int i;

	/*
	 *  empty the buffer
	 */
	kdbdly(20);
	while(KBDCTL & Sobf){
		i = KBDDAT;
		USED(i);
		kdbdly(1);
	}
}

/*
 *  send a command to the mouse
 */
static int
mousecmd(int cmd)
{
	int tries;
	unsigned int c;

	c = 0;
	tries = 0;
	do{
		if(tries++ > 2)
			break;
		OUTWAIT;
		KBDCTL = 0xD4;
		OUTWAIT;
		KBDDAT = cmd;
		OUTWAIT;
		kbdwait();
		c = KBDDAT;
	} while(c == 0xFE || c == 0);
	if(c != 0xFA){
		print("mouse returns %2.2ux to the %2.2ux command\n", c, cmd);
		return -1;
	}
	return 0;
}

int
kbdinit(void)
{
	int i;

	/*
	 *  empty the buffer
	 */
	while(KBDCTL & Sobf){
		i = KBDDAT;
		USED(i);
	}


	/*
	 *  disable the interface
	 */
	OUTWAIT;
	KBDCTL = Kwrcmd;
	OUTWAIT;
	KBDDAT = Csystem | Cinhibit | Cdisable | Cintena;

	/*
	 *  set unix scan on the keyboard
	 */
	OUTWAIT;
	KBDDAT = Mdisable;
	INWAIT;
	if(KBDDAT != Rack)
		return 0;
	OUTWAIT;
	KBDDAT = Msetscan;
	ACKWAIT;
	OUTWAIT;
	KBDDAT = 0;
	ACKWAIT;
	OUTWAIT;
	KBDDAT = Menable;

	/*
	 *  enable the interface
	 */
	OUTWAIT;
	KBDCTL = Kwrcmd;
	OUTWAIT;
	KBDDAT = Csystem | Cinhibit | Cintena | Cmseint;
	OUTWAIT;
	KBDCTL = Kenable;
	OUTWAIT;
	KBDCTL = Kmseena;
	empty();

	mousecmd(0xEA);
	mousecmd(0xF4);

	return 1;
}
