#include "../port/portfns.h"

void	cacheinit(void);
void	clearfpintr(void);
#define	clearmmucache()
void	clockinit(void);
void	evenaddr(ulong);
char*	excname(ulong);
void	faultasync(Ureg*);
void	faultsparc(Ureg*);
void	flushcpucache(void);
#define	flushpage(x)
int	fpcr(int);
void	fpregrestore(char*);
void	fpregsave(char*);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
int	getb2(ulong);
int	getw2(ulong);
void	intrinit(void);
void	ioinit(void);
int	kbdstate(IOQ*, int);
void	kbdclock(void);
void	kbdrepeat(int);
KMap*	kmap(Page*);
void	kmapinit(void);
KMap*	kmappa(ulong, ulong);
int	kprint(char*, ...);
void	kproftimer(ulong);
void	kunmap(KMap*);
void	lanceintr(void);
void	lancesetup(Lance*);
void	lancetoggle(void);
void	mmuinit(void);
void	mousebuttons(int);
void	printinit(void);
#define	procrestore(x,y)
#define	procsave(x,y)
#define	procsetup(x)	((p)->fpstate = FPinit)
void	putb2(ulong, int);
void	putcontext(int);
void	putcxreg(int);
void	putcxsegm(int, ulong, int);
void	putpmeg(ulong, ulong);
void	putsegm(ulong, int);
void	putstr(char*);
void	putstrn(char*, long);
void	puttbr(ulong);
void	putw2(ulong, ulong);
void	putw4(ulong, ulong);
void	putwC(ulong, ulong);
void	putwD16(ulong, ulong);
void	putwD(ulong, ulong);
void	putwE16(ulong, ulong);
void	putwE(ulong, ulong);
void	systemreset(void);
void	restartprint(Alarm*);
void	restfpregs(FPsave*);
void	screeninit(void);
void	screenputc(int);
void	screenputs(char*, int);
void	spldone(void);
ulong	swap1(ulong*);
void	touser(ulong);
void	trap(Ureg*);
void	trapinit(void);
#define	wbflush()	/* mips compatibility */
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
