#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

enum {
	Data=		0x60,	/* data port */

	Status=		0x64,	/* status port */
	 Inready=	0x01,	/*  input character ready */
	 Outbusy=	0x02,	/*  output busy */
	 Sysflag=	0x04,	/*  ??? */
	 Cmddata=	0x08,	/*  cmd==0, data==1 */
	 kbdinh=	0x10,	/*  keyboard inhibited */
	 Xtimeout=	0x20,	/*  transmit timeout */
	 Rtimeout=	0x40,	/*  receive timeout */
	 Parity=	0x80,	/*  0==odd, 1==even */

	Spec=	0x80,

	PF=	Spec|0x20,	/* num pad function key */
	View=	Spec|0x00,	/* view (shift window up) */
	F=	Spec|0x40,	/* function key */
	Shift=	Spec|0x60,
	Break=	Spec|0x61,
	Ctrl=	Spec|0x62,
	Latin=	Spec|0x63,
	Caps=	Spec|0x64,
	Num=	Spec|0x65,
	No=	Spec|0x7F,	/* no mapping */

	Home=	F|13,
	Up=	F|14,
	Pgup=	F|15,
	Print=	F|16,
	Left=	View,
	Right=	View,
	End=	'\r',
	Down=	View,
	Pgdown=	View,
	Ins=	F|20,
	Del=	0x7F,
};

uchar kbtab[] = 
{
[0x00]	No,	0x1b,	'1',	'2',	'3',	'4',	'5',	'6',
[0x08]	'7',	'8',	'9',	'0',	'-',	'=',	'\b',	'\t',
[0x10]	'q',	'w',	'e',	'r',	't',	'y',	'u',	'i',
[0x18]	'o',	'p',	'[',	']',	'\n',	Ctrl,	'a',	's',
[0x20]	'd',	'f',	'g',	'h',	'j',	'k',	'l',	';',
[0x28]	'\'',	'`',	Shift,	'\\',	'z',	'x',	'c',	'v',
[0x30]	'b',	'n',	'm',	',',	'.',	'/',	Shift,	No,
[0x38]	Latin,	' ',	Caps,	F|1,	F|2,	F|3,	F|4,	F|5,
[0x40]	F|6,	F|7,	F|8,	F|9,	F|10,	Num,	F|12,	Home,
[0x48]	No,	No,	No,	No,	No,	No,	No,	No,
[0x50]	No,	No,	No,	No,	No,	No,	No,	F|11,
[0x58]	F|12,	No,	No,	No,	No,	No,	No,	No,
};

uchar kbtabshift[] =
{
[0x00]	No,	0x1b,	'!',	'@',	'#',	'$',	'%',	'^',
[0x08]	'&',	'*',	'(',	')',	'_',	'+',	'\b',	'\t',
[0x10]	'Q',	'W',	'E',	'R',	'T',	'Y',	'U',	'I',
[0x18]	'O',	'P',	'{',	'}',	'\n',	Ctrl,	'A',	'S',
[0x20]	'D',	'F',	'G',	'H',	'J',	'K',	'L',	':',
[0x28]	'"',	'~',	Shift,	'|',	'Z',	'X',	'C',	'V',
[0x30]	'B',	'N',	'M',	'<',	'>',	'?',	Shift,	No,
[0x38]	Latin,	' ',	Caps,	F|1,	F|2,	F|3,	F|4,	F|5,
[0x40]	F|6,	F|7,	F|8,	F|9,	F|10,	Num,	F|12,	Home,
[0x48]	No,	No,	No,	No,	No,	No,	No,	No,
[0x50]	No,	No,	No,	No,	No,	No,	No,	F|11,
[0x58]	F|12,	No,	No,	No,	No,	No,	No,	No,
};

uchar kbtabesc1[] =
{
[0x00]	No,	No,	No,	No,	No,	No,	No,	No,
[0x08]	No,	No,	No,	No,	No,	No,	No,	No,
[0x10]	No,	No,	No,	No,	No,	No,	No,	No,
[0x18]	No,	No,	No,	No,	No,	Ctrl,	No,	No,
[0x20]	No,	No,	No,	No,	No,	No,	No,	No,
[0x28]	No,	No,	No,	No,	No,	No,	No,	No,
[0x30]	No,	No,	No,	No,	No,	No,	No,	Print,
[0x38]	Latin,	No,	No,	No,	No,	No,	No,	No,
[0x40]	No,	No,	No,	No,	No,	No,	Break,	Home,
[0x48]	Up,	Pgup,	No,	Down,	No,	Right,	No,	End,
[0x50]	Left,	Pgdown,	Ins,	Del,	No,	No,	No,	No,
[0x58]	No,	No,	No,	No,	No,	No,	No,	No,
};

struct latin
{
	uchar	l;
	char	c[2];
}latintab[] = {
	'',	"!!",	/* spanish initial ! */
	'',	"c|",	/* cent */
	'',	"c$",	/* cent */
	'',	"l$",	/* pound sterling */
	'',	"g$",	/* general currency */
	'',	"y$",	/* yen */
	'',	"j$",	/* yen */
	'',	"||",	/* broken vertical bar */
	'',	"SS",	/* section symbol */
	'',	"\"\"",	/* dieresis */
	'',	"cr",	/* copyright */
	'',	"cO",	/* copyright */
	'',	"sa",	/* super a, feminine ordinal */
	'',	"<<",	/* left angle quotation */
	'',	"no",	/* not sign, hooked overbar */
	'',	"--",	/* soft hyphen */
	'',	"rg",	/* registered trademark */
	'',	"__",	/* macron */
	'',	"s0",	/* degree (sup o) */
	'',	"+-",	/* plus-minus */
	'',	"s2",	/* sup 2 */
	'',	"s3",	/* sup 3 */
	'',	"''",	/* grave accent */
	'',	"mu",	/* mu */
	'',	"pg",	/* paragraph (pilcrow) */
	'',	"..",	/* centered . */
	'',	",,",	/* cedilla */
	'',	"s1",	/* sup 1 */
	'',	"so",	/* sup o */
	'',	">>",	/* right angle quotation */
	'',	"14",	/* 1/4 */
	'',	"12",	/* 1/2 */
	'',	"34",	/* 3/4 */
	'',	"??",	/* spanish initial ? */
	'',	"A`",	/* A grave */
	'',	"A'",	/* A acute */
	'',	"A^",	/* A circumflex */
	'',	"A~",	/* A tilde */
	'',	"A\"",	/* A dieresis */
	'',	"A:",	/* A dieresis */
	'',	"Ao",	/* A circle */
	'',	"AO",	/* A circle */
	'',	"Ae",	/* AE ligature */
	'',	"AE",	/* AE ligature */
	'',	"C,",	/* C cedilla */
	'',	"E`",	/* E grave */
	'',	"E'",	/* E acute */
	'',	"E^",	/* E circumflex */
	'',	"E\"",	/* E dieresis */
	'',	"E:",	/* E dieresis */
	'',	"I`",	/* I grave */
	'',	"I'",	/* I acute */
	'',	"I^",	/* I circumflex */
	'',	"I\"",	/* I dieresis */
	'',	"I:",	/* I dieresis */
	'',	"D-",	/* Eth */
	'',	"N~",	/* N tilde */
	'',	"O`",	/* O grave */
	'',	"O'",	/* O acute */
	'',	"O^",	/* O circumflex */
	'',	"O~",	/* O tilde */
	'',	"O\"",	/* O dieresis */
	'',	"O:",	/* O dieresis */
	'',	"OE",	/* O dieresis */
	'',	"Oe",	/* O dieresis */
	'',	"xx",	/* times sign */
	'',	"O/",	/* O slash */
	'',	"U`",	/* U grave */
	'',	"U'",	/* U acute */
	'',	"U^",	/* U circumflex */
	'',	"U\"",	/* U dieresis */
	'',	"U:",	/* U dieresis */
	'',	"UE",	/* U dieresis */
	'',	"Ue",	/* U dieresis */
	'',	"Y'",	/* Y acute */
	'',	"P|",	/* Thorn */
	'',	"Th",	/* Thorn */
	'',	"TH",	/* Thorn */
	'',	"ss",	/* sharp s */
	'',	"a`",	/* a grave */
	'',	"a'",	/* a acute */
	'',	"a^",	/* a circumflex */
	'',	"a~",	/* a tilde */
	'',	"a\"",	/* a dieresis */
	'',	"a:",	/* a dieresis */
	'',	"ao",	/* a circle */
	'',	"ae",	/* ae ligature */
	'',	"c,",	/* c cedilla */
	'',	"e`",	/* e grave */
	'',	"e'",	/* e acute */
	'',	"e^",	/* e circumflex */
	'',	"e\"",	/* e dieresis */
	'',	"e:",	/* e dieresis */
	'',	"i`",	/* i grave */
	'',	"i'",	/* i acute */
	'',	"i^",	/* i circumflex */
	'',	"i\"",	/* i dieresis */
	'',	"i:",	/* i dieresis */
	'',	"d-",	/* eth */
	'',	"n~",	/* n tilde */
	'',	"o`",	/* o grave */
	'',	"o'",	/* o acute */
	'',	"o^",	/* o circumflex */
	'',	"o~",	/* o tilde */
	'',	"o\"",	/* o dieresis */
	'',	"o:",	/* o dieresis */
	'',	"oe",	/* o dieresis */
	'',	"-:",	/* divide sign */
	'',	"o/",	/* o slash */
	'',	"u`",	/* u grave */
	'',	"u'",	/* u acute */
	'',	"u^",	/* u circumflex */
	'',	"u\"",	/* u dieresis */
	'',	"u:",	/* u dieresis */
	'',	"ue",	/* u dieresis */
	'',	"y'",	/* y acute */
	'',	"th",	/* thorn */
	'',	"p|",	/* thorn */
	'',	"y\"",	/* y dieresis */
	'',	"y:",	/* y dieresis */
	0,	0,
};

KIOQ	kbdq;

int
latin1(int k1, int k2)
{
	int i;
	struct latin *l;

	for(l=latintab; l->l; l++)
		if(k1==l->c[0] && k2==l->c[1])
			return l->l;
	return 0;
}

void
kbdinit(void)
{
	initq(&kbdq);
	setvec(Kbdvec, kbdintr, SEGIG);
}

int
kbdputc(IOQ* q, int c)
{
	if(c==0x10)
		panic("^p");
	putc(q, c);
}

/*
 *  keyboard interrupt
 */
void
kbdintr(Ureg *ur)
{
	int c, nc;
	static int esc1, esc2;
	static int shift;
	static int caps;
	static int ctl;
	static int num;
	static int lstate, k1, k2;
	int keyup;

	/*
	 *  get a character to be there
	 */
	c = inb(Data);

	keyup = c&0x80;
	c &= 0x7f;
	if(c > sizeof kbtab){
		print("unknown key %ux\n", c|keyup);
		kbdputc(&kbdq, k1);
		return;
	}

	/*
	 *  e0's is the first of a 2 character sequence
	 */
	if(c == 0xe0){
		esc1 = 1;
		return;
	} else if(c == 0xe1){
		esc2 = 2;
		return;
	}

	if(esc1){
		c = kbtabesc1[c];
		esc1 = 0;
	} else if(esc2){
		esc2--;
		return;
	} else if(shift)
		c = kbtabshift[c];
	else
		c = kbtab[c];

	if(caps && c<='z' && c>='a')
		c += 'A' - 'a';

	/*
	 *  keyup only important for shifts
	 */
	if(keyup){
		switch(c){
		case Shift:
			shift = 0;
			break;
		case Ctrl:
			ctl = 0;
			break;
		}
		return;
	}

	/*
 	 *  normal character
	 */
	if(!(c & Spec)){
		if(ctl)
			c &= 0x1f;
		switch(lstate){
		case 1:
			k1 = c;
			lstate = 2;
			return;
		case 2:
			k2 = c;
			lstate = 0;
			c = latin1(k1, k2);
			if(c == 0){
				kbdputc(&kbdq, k1);
				c = k2;
			}
			/* fall through */
		default:
			break;
		}
	} else {
		switch(c){
		case Caps:
			caps ^= 1;
			return;
		case Num:
			num ^= 1;
			return;
		case Shift:
			shift = 1;
			return;
		case Latin:
			lstate = 1;
			return;
		case Ctrl:
			ctl = 1;
			return;
		}
	}
	kbdputc(&kbdq, c);
	return;
}
