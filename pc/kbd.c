#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"

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

	
};

char noshift[256] = 
{
[0x00]	Nokey,	0x1b,	'1',	'2',	'3',	'4',	'5',	'6',
[0x08]	'7',	'8',	'9',	'0',	'-',	'=',	'\b',	'\t',
[0x10]
[0x28]
[0x30]
};

/*
 *  get a byte from the keyboard
 */
int
kbdc(void)
{
	while((inb(Status)&Inready)==0)
		;
	return inb(Data);
}

1	0x1b,
3b	F1,
3c	F2,
3d	F3,
3e	F4,
3f	F5,
40	f6,
41	F7,
42	F8,
43	F9,
44	F10,
57	F11,
58	F12,
e0 52	INS,
e0 53	DEL,
2	'1',
3	'2',
4	'3',
5	'4',
6	'5',
7	'6',
8	'7',
9	'8',
A	'9',
B	'0',
C	'-',
D	'=',
E	'\b',
E0 47	Home,
F	'\t',
10	'q',
11	'w',
12	'e',
13	'r',
14	't',
15	'y',
16	'u',
17	'i',
18	'o',
19	'p',
1a	'[',
1b	']',
2b	'\\',
e0 49	Pageup,
3a	Capslock,
1e	'a'
1f	's' 
20	'd'
21	'f'
22	'g'
23	'h'
24	'j'
25	'k'
26	'l'
27	';'
28	'\''
1c	'\r'
2a	Lshift,
2c	'z'
2d	'x'
2e	'c'
2f	'v'
30	'b'
31	'n'
32	'm'
33	','
34	'.'
35	'/'
36	Rshift,
e0 51	Pagedown,
e0 4f	End,
e0 48	Uparrow,
e0 50	Downarrow,
e0 4b	Leftarrow,
e0 4d   Rightarrow,
e0 1d	Rctl,
e0 38	Ralt,
39	Space,
38	Lalt,
1d	Lctl,
29	'`',
1d 38 1 81 b8 9d	F1
1d 38 21 a1 b8 9d	F2
	2e ae		F3
	19 99		F4
	26		F5
	2F		F6

e1 1d 45 e1 9d cf	F7
e0 46 e0 c6		f8
e0 2a e0 37 e0 b7 e0 aa	f9
54			f10
45			f11
46			f12
