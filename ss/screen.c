#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	<libg.h>
#include	<gnot.h>
#include	"screen.h"

#define	MINX	8

extern	GSubfont	defont0;
GSubfont		*defont;

struct{
	Point	pos;
	int	bwid;
}out;

void	(*kprofp)(ulong);

GBitmap gscreen;

struct screens
{
	ulong	type;
	int	x;
	int	y;
	int	ld;
}screens[] = {
	{ 0xFE010104, 1152, 900, 0 },
	0
};

Lock screenlock;

void
screeninit(void)
{
	struct screens *s;

	for(s=screens; s->type; s++)
		if(s->type == conf.monitor)
			goto found;
	/* default is 0th element of table */
	if(conf.monitor){
		s = screens;
		goto found;
	}
	conf.monitor = 0;
	return;

    found:
	gscreen.base = (ulong*)SCREENSEGM;
	gscreen.zero = 0;
	gscreen.width = (s->x<<s->ld)/32;
	gscreen.ldepth = s->ld;
	gscreen.r = Rect(0, 0, s->x, s->y);
	gscreen.clipr = gscreen.r;
	gscreen.cache = 0;
	defont = &defont0;
	gbitblt(&gscreen, Pt(0, 0), &gscreen, gscreen.r, 0);
	out.pos.x = MINX;
	out.pos.y = 0;
	out.bwid = defont0.info[' '].width;
}

void
screenputnl(void)
{
	if(!conf.monitor)
		return;
	out.pos.x = MINX;
	out.pos.y += defont0.height;
	if(out.pos.y > gscreen.r.max.y-defont0.height)
		out.pos.y = gscreen.r.min.y;
	gbitblt(&gscreen, Pt(0, out.pos.y), &gscreen,
	    Rect(0, out.pos.y, gscreen.r.max.x, out.pos.y+2*defont0.height), 0);
}

void
screenputs(char *s, int n)
{
	Rune r;
	int i;
	char buf[4];

	if(!conf.monitor)
		return;
	lock(&screenlock);
	while(n > 0){
		i = chartorune(&r, s);
		if(i == 0){
			s++;
			--n;
			continue;
		}
		memmove(buf, s, i);
		buf[i] = 0;
		n -= i;
		s += i;
		if(r == '\n')
			screenputnl();
		else if(r == '\t'){
			out.pos.x += (8-((out.pos.x-MINX)/out.bwid&7))*out.bwid;
			if(out.pos.x >= gscreen.r.max.x)
				screenputnl();
		}else if(r == '\b'){
			if(out.pos.x >= out.bwid+MINX){
				out.pos.x -= out.bwid;
				gsubfstring(&gscreen, out.pos, defont, " ", S);
			}
		}else{
			if(out.pos.x >= gscreen.r.max.x-out.bwid)
				screenputnl();
			out.pos = gsubfstring(&gscreen, out.pos, defont, buf, S);
		}
	}
	unlock(&screenlock);
}

/*
 * Map is indexed by keyboard char, output is ASCII.
 * Gnotisms: Return sends newline and Line Feed sends carriage return.
 * Delete and Backspace both send backspace.
 * Num Lock sends delete (rubout).
 * Alt Graph is VIEW (scroll).
 * Compose builds Latin-1 characters.
 */
uchar keymap[128] = {
/*	00    L1    02    L2    04    F1    F2    07	*/
	0xFF, 0x80, 0xFF, 0x81, 0xFF, 0x82, 0x83, 0xFF,
/*	F3    09    F4    0b    F5    altgr F6    0f  	*/
	0x84, 0xFF, 0x85, 0xFF, 0x86, 0x80, 0x87, 0xFF,
/*	F7    F8    F9    Alt   14    R1    R2    R3	*/
	0x88, 0x89, 0x8a, 0x8b, 0xFF, 0x8c, 0x8d, 0x8e,
/*	18    L3    L4    1b    1c    Esc   1     2	*/
	0xFF, 0x8f, 0x90, 0xFF, 0xFF, 0x1b, '1',  '2',
/*	3     4     5     6     7     8     9     0	*/
	'3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',
/*	-     =     `     bs    2c    R4    R5    R6	*/
	'-',  '=',  '`',  '\b', 0xFF, 0x91, 0x92, 0x93,
/*	30    L5    del   L6   34    tab   q     w  	*/
	0xFF, 0x94, 0xFF, 0x95, 0xFF, '\t', 'q',  'w',
/*	e     r     t     y     u     i     o     p    	*/
	'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',
/*	[     ]     dele  comp  R7    R8    R9    r -	*/
	'[',  ']',  '\b', 0xB6, 0x96, 0x97, 0x98, 0xFF,
/*	L7    L8    4a    4b    ctrl  a     s     d	*/
	0x99, 0x9a, 0xFF, 0xFF, 0xF0, 'a',  's',  'd',
/*	f     g     h     j     k     l     ;     '   	*/
	'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'',
/*	\     ret   enter R10   R11   R12   ins   L9	*/
	'\\', '\n', 0xFF, 0x9b, 0x9c, 0x9d, 0xFF, 0x9e, 
/*	60    L10   numlk shift z     x     c     v	*/
	0xFF, 0x9f, 0x7F, 0xF1, 'z',  'x',  'c', 'v',
/*	b     n     m     ,     .     /     shift lf	*/
	'b',  'n',  'm',  ',',  '.',  '/',  0xF1, '\r',
/*	R13   R14   R15   73    74    75    help  caps	*/
	0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2,
/*	lloz  79    rloz  7b    7c    r +   7e    7f	*/
	0xA3, ' ',  0xA4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

uchar keymapshift[128] = {
/*	00    L1    02    L2    04    F1    F2    07	*/
	0xFF, 0x80, 0xFF, 0x81, 0xFF, 0x82, 0x83, 0xFF,
/*	F3    09    F4    0b    F5    altgr F6    0f  	*/
	0x84, 0xFF, 0x85, 0xFF, 0x86, 0x80, 0x87, 0xFF,
/*	F7    F8    F9    Alt   14    R1    R2    R3	*/
	0x88, 0x89, 0x8a, 0x8b, 0xFF, 0x8c, 0x8d, 0x8e,
/*	18    L3    L4    1b    1c    Esc   1     2	*/
	0xFF, 0x8f, 0x90, 0xFF, 0xFF, 0x1b, '!',  '@',
/*	3     4     5     6     7     8     9     0	*/
	'#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
/*	-     =     `     bs    2c    R4    R5    R6	*/
	'_',  '+',  '~',  '\b', 0xFF, 0x91, 0x92, 0x93,
/*	30    L5    del   L6    34    tab   q     w  	*/
	0xFF, 0x94, 0xFF, 0x95, 0xFF, '\t', 'Q',  'W',
/*	e     r     t     y     u     i     o     p    	*/
	'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',
/*	[     ]     dele  comp  R7    R8    R9    r -	*/
	'{',  '}',  '\b', 0xB6, 0x96, 0x97, 0x98, 0xFF,
/*	L7    L8    4a    4b    ctrl  a     s     d	*/
	0x99, 0x9a, 0xFF, 0xFF, 0xF0, 'A',  'S',  'D',
/*	f     g     h     j     k     l     ;     '   	*/
	'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',
/*	\     ret   enter R10   R11   R12   ins   L9	*/
	'|', '\n',  0xFF, 0x9b, 0x9c, 0x9d, 0xFF, 0x9e, 
/*	60    L10   numlk shift z     x     c     v	*/
	0xFF, 0x9f, 0x7F, 0xF1, 'Z',  'X',  'C', 'V',
/*	b     n     m     ,     .     /     shift lf	*/
	'B',  'N',  'M',  '<',  '>',  '?',  0xF1, '\r',
/*	R13   R14   R15   73    74    75    help  caps	*/
	0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2,
/*	lloz  79    rloz  7b    7c    r +  7e    7f	*/
	0xA3, ' ',  0xA4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

uchar keymapctrl[128] = {
/*	00    L1    02    L2    04    F1    F2    07	*/
	0xFF, 0x80, 0xFF, 0x81, 0xFF, 0x82, 0x83, 0xFF,
/*	F3    09    F4    0b    F5    altgr F6    0f  	*/
	0x84, 0xFF, 0x85, 0xFF, 0x86, 0x80, 0x87, 0xFF,
/*	F7    F8    F9    Alt   14    R1    R2    R3	*/
	0x88, 0x89, 0x8a, 0x8b, 0xFF, 0x8c, 0x8d, 0x8e,
/*	18    L3    L4    1b    1c    Esc   1     2	*/
	0xFF, 0x8f, 0x90, 0xFF, 0xFF, 0x1b, '!',  '@',
/*	3     4     5     6     7     8     9     0	*/
	'#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
/*	-     =     `     bs    2c    R4    R5    R6	*/
	'_',  '+',  '~', '\b', 0xFF, 0x91, 0x92, 0x93,
/*	30    L5    del   L6    34    tab   q     w  	*/
	0xFF, 0x94, 0xFF, 0x95, 0xFF, '\t', 0x11, 0x17,
/*	e     r     t     y     u     i     o     p    	*/
	0x05, 0x12, 0x14, 0x19, 0x15, 0x09, 0x0F, 0x10,
/*	[     ]     dele  comp  R7    R8    R9    r -	*/
	0x1B, 0x1D, '\b', 0xB6, 0x96, 0x97, 0x98, 0xFF,
/*	L7    L8    4a    4b    ctrl  a     s     d	*/
	0x99, 0x9a, 0xFF, 0xFF, 0xF0, 0x01, 0x13, 0x04,
/*	f     g     h     j     k     l     ;     '   	*/
	0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C,':',  '"',
/*	\     ret   enter R10   R11   R12   ins   L9	*/
	0x1C, '\n',  0xFF, 0x9b, 0x9c, 0x9d, 0xFF, 0x9e, 
/*	60    L10   numlk shift z     x     c     v	*/
	0xFF, 0x9f, 0x7F, 0xF1, 0x1A, 0x18, 0x03, 0x16,
/*	b     n     m     ,     .     /     shift lf	*/
	0x02, 0x0E, 0x0D, '<',  '>',  '?',  0xF1, '\r',
/*	R13   R14   R15   73    74    75    help  caps	*/
	0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2,
/*	lloz  79    rloz  7b    7c    r +  7e    7f	*/
	0xA3, ' ',  0xA4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

uchar keymapshiftctrl[128] = {
/*	00    L1    02    L2    04    F1    F2    07	*/
	0xFF, 0x80, 0xFF, 0x81, 0xFF, 0x82, 0x83, 0xFF,
/*	F3    09    F4    0b    F5    altgr F6    0f  	*/
	0x84, 0xFF, 0x85, 0xFF, 0x86, 0x80, 0x87, 0xFF,
/*	F7    F8    F9    Alt   14    R1    R2    R3	*/
	0x88, 0x89, 0x8a, 0x8b, 0xFF, 0x8c, 0x8d, 0x8e,
/*	18    L3    L4    1b    1c    Esc   1     2	*/
	0xFF, 0x8f, 0x90, 0xFF, 0xFF, 0x1b, '!',  0x00,
/*	3     4     5     6     7     8     9     0	*/
	'#',  '$',  '%',  0x1E, '&',  '*',  '(',  ')',
/*	-     =     `     bs    2c    R4    R5    R6	*/
	0x1F, '+',  '~', '\b', 0xFF, 0x91, 0x92, 0x93,
/*	30    L5    del   L6     34    tab   q     w  	*/
	0xFF, 0x94, 0xFF, 0x95, 0xFF, '\t', 0x11, 0x17,
/*	e     r     t     y     u     i     o     p    	*/
	0x05, 0x12, 0x14, 0x19, 0x15, 0x09, 0x0F, 0x10,
/*	[     ]     dele  comp  R7    R8    R9    r -	*/
	0x1B, 0x1D, '\b', 0xB6, 0x96, 0x97, 0x98, 0xFF,
/*	L7    L8    4a    4b    ctrl  a     s     d	*/
	0x99, 0x9a, 0xFF, 0xFF, 0xF0, 0x01, 0x13, 0x04,
/*	f     g     h     j     k     l     ;     '   	*/
	0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C,':',  '"',
/*	\     ret   enter R10   R11   R12   ins   L9	*/
	0x1C, '\n', 0xFF, 0x9b, 0x9c, 0x9d, 0xFF, 0x9e, 
/*	60    L10   numlk shift z     x     c     v	*/
	0xFF, 0x9f, 0x7F, 0xF1, 0x1A, 0x18, 0x03, 0x16,
/*	b     n     m     ,     .     /     shift lf	*/
	0x02, 0x0E, 0x0D, '<',  '>',  '?',  0xF1, '\r',
/*	R13   R14   R15   73    74    75    help  caps	*/
	0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2,
/*	lloz  79    rloz  7b    7c    r +  7e    7f	*/
	0xA3, ' ',  0xA4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static uchar *kbdmap[4] = {
	keymap,
	keymapshift,
	keymapctrl,
	keymapshiftctrl
};

int
kbdstate(IOQ *q, int c)
{
	static shift = 0x00;
	static caps = 0;
	static long startclick;
	static int repeatc;
	static int lstate;
	static uchar kc[4];
	uchar ch;
	int i, nk;

	USED(q);
	ch = kbdmap[shift][c&0x7F];
	if(c==0x7F){	/* all keys up */
    norepeat:
		kbdrepeat(0);
		return 0;
	}
	if(ch == 0xFF)	/* shouldn't happen; ignore */
		return 0;
	if(c & 0x80){	/* key went up */
		if(ch == 0xF0){		/* control */
			shift &= ~2;
			goto norepeat;
		}
		if(ch == 0xF1){	/* shift */
			shift &= ~1;
			goto norepeat;
		}
		if(ch == 0xF2){	/* caps */
			goto norepeat;
		}
		goto norepeat;
	}
	if(ch == 0xF0){		/* control */
		shift |= 2;
		goto norepeat;
	}
	if(ch==0xF1){	/* shift */
		shift |= 1;
		goto norepeat;
	}
	if(ch==0xF2){	/* caps */
		caps ^= 1;
		goto norepeat;
	}
	if(caps && 'a'<=ch && ch<='z')
		ch |= ' ';
	repeatc = ch;
	kbdrepeat(1);
	if(ch == 0xB6)	/* Compose */
		lstate = 1;
	else{
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
				kbdputc(&kbdq, c);
			else for(i=0; i<nk; i++)
				kbdputc(&kbdq, kc[i]);
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
			kbdputc(&kbdq, ch);
			break;
		}
	}
	return 0;
}

void
buzz(int freq, int dur)
{
	USED(freq, dur);
}

void
lights(int mask)
{
	USED(mask);
}

int
screenbits(void)
{
	return 1;	/* bits per pixel */
}

void
getcolor(ulong p, ulong *pr, ulong *pg, ulong *pb)
{
	ulong ans;

	/*
	 * The slc monochrome says 0 is white (max intensity)
	 */
	if(p == 0)
		ans = ~0;
	else
		ans = 0;
	*pr = *pg = *pb = ans;
}


int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p, r, g, b);
	return 0;	/* can't change mono screen colormap */
}

int
hwcursset(uchar *s, uchar *c, int ox, int oy)
{
	USED(s, c, ox, oy);
	return 0;
}

int
hwcursmove(int x, int y)
{
	USED(x, y);
	return 0;
}

void
mouseclock(void)	/* called splhi */
{
	mouseupdate(1);
}
