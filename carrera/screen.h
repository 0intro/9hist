typedef struct Cursorinfo	Cursorinfo;

struct Cursorinfo
{
	Cursor;
	Lock;
};

extern Cursorinfo	cursor;
extern Cursor		arrow;

extern int	cursoron(int);
#define		cursoroff(x)
extern Point	mousexy(void);
void		setcursor(Cursor*);

void		mousectl(char*);
void		mousetrack(int, int, int);
