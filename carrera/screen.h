typedef struct Cursorinfo	Cursorinfo;

struct Cursorinfo
{
	Cursor;
	Lock;
};

extern Cursorinfo	cursor;

extern void	mouseupdate(int);
extern void	cursoron(int);
#define		cursoroff(x)
#define		mousectl(s)
extern Point	mousexy(void);
#define		hwscreenwrite(a, b)
