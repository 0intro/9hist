typedef struct Cursorinfo Cursorinfo;
typedef struct Cursor Cursor;

struct Cursorinfo {
	Lock;
};

extern void	blankscreen(int);
extern void	flushmemscreen(Rectangle);
