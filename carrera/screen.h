typedef struct Cursorinfo	Cursorinfo;

struct Cursorinfo
{
	Cursor;
	Lock;
};

extern Cursorinfo	cursor;

extern void	mouseupdate(int);
extern int	cursoron(int);
#define		cursoroff(x)
extern Point	mousexy(void);
void			setcursor(Cursor*);
