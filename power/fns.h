#include "../port/portfns.h"

void	arginit(void);
int	blen(Block *);
int	bround(Block *, int);
void	clearmmucache(void);
void	clockinit(void);
ulong	confeval(char*);
void	confread(void);
void	confprint(void);
void	confset(char*);
int	consactive(void);
int	conschar(void);
void	consoff(void);
int	consputc(int);
void	duartinit(void);
void	duartintr(void);
int	duartputc(int);
void	duartputs(char*);
void	duartreset(void);
void	duartxmit(int);
void	evenaddr(ulong);
void	faultmips(Ureg*, int, int);
ulong	fcr31(void);
void	flushmmu(void);
#define	flushpage(x)
#define	flushvirt()
void	gettlb(int, ulong*);
ulong	gettlbvirt(int);
void	gotopc(ulong);
void	icflush(void *, int);
void	ioboardinit(void);
void	kbdchar(int);
void	intr(Ureg*);
void	lanceintr(void);
void	lanceparity(void);
void	lancesetup(Lance*);
void	launchinit(void);
void	launch(int);
void	lights(int);
void	newstart(void);
int	newtlbpid(Proc*);
void	novme(int);
void	online(void);
void	printinit(void);
Block*	prepend(Block*, int);
void	prflush(void);
#define procsetup(p)	((p)->fpstate = FPinit)
#define procsave(x,y)
#define procrestore(x,y)
Block*	pullup(Block *, int);
void	purgetlb(int);
void	putstlb(ulong, ulong);
void	putstrn(char*, long);
void	puttlb(ulong, ulong);
void	puttlbx(int, ulong, ulong);
int	readlog(ulong, char*, ulong);
void	restfpregs(FPsave*, ulong);
void	setvmevec(int, void (*)(int));
void	sinit(void);
uchar*	smap(int, uchar*);
void	sunmap(int, uchar*);
void	syslog(char*, int);
void	sysloginit(void);
void	tlbinit(void);
Block*	tolance(Block*, int);
void	touser(void*);
void	vecinit(void);
void	vector0(void);
void	vector80(void);
void	vmereset(void);
void	wbflush(void);
#define	waserror()	setlabel(&u->errlab[u->nerrlab++])
