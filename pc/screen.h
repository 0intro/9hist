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
extern int	cursoron(int);
extern void	cursoroff(int);
extern Point	mousexy(void);
extern void	cursorinit(void);
extern void	setcursor(Cursor*);
