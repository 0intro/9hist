typedef struct Cursorinfo	Cursorinfo;

struct Cursorinfo
{
	Cursor;
	Lock;
	int	visible;	/* on screen */
	int	disable;	/* from being used */
	int	frozen;		/* from being used */
	Rectangle r;		/* location */
	Rectangle clipr;	/* r clipped into screen */
	int	l;		/* width of cursorwork (in bytes) */
	int	tl;		/* scan line byte width of mouse at r */
};

Cursorinfo	cursor;
Cursor		curs;
extern int	cursoron(int);
extern void	cursoroff(int);
extern void	setcursor(Cursor*);

/*
 *  mouse types
 */
enum
{
	Mouseother=	0,
	Mouseserial=	1,
	MousePS2=	2,
};
extern int	mousetype;

extern void	mousectl(char*);
extern void	mousetrack(int, int, int);
extern Point	mousexy(void);

extern void	mouseaccelerate(char*);
extern int	m3mouseputc(void*, int);
extern int	mouseputc(void*, int);
extern int	mouseswap;
