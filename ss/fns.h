#include "../port/portfns.h"

void	cacheinit(void);
void	clearfpintr(void);
#define	clearmmucache()
void	clockinit(void);
void	duartbaud(int);
void	duartbreak(int);
void	duartdtr(int);
void	duartinit(void);
int	duartinputport(void);
void	duartintr(void);
void	duartstartrs232o(void);
void	duartstarttimer(void);
void	duartstoptimer(void);
void	evenaddr(ulong);
char*	excname(ulong);
void	faultasync(Ureg*);
void	faultsparc(Ureg*);
void	flushcpucache(void);
#define	flushpage(x)
#define	flushvirt()	flushmmu()
int	fpcr(int);
void	fpregrestore(char*);
void	fpregsave(char*);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
int	getb2(ulong);
int	getrs232o(void);
int	getw2(ulong);
void	intrinit(void);
void	kbdchar(int);
void	kbdclock(void);
void	kbdrepeat(int);
KMap*	kmap(Page*);
void	kmapinit(void);
KMap*	kmappa(ulong, ulong);
int	kprint(char*, ...);
void	kunmap(KMap*);
void	lanceintr(void);
void	lancesetup(Lance*);
void	lancetoggle(void);
void	mmuinit(void);
void	mousebuttons(int);
void	mousechar(int);
void	mouseclock(void);
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
void	reset(void);
void	restartprint(Alarm*);
void	restfpregs(FPsave*);
void	rs232ichar(int);
void	screeninit(void);
void	screenputc(int);
void	spldone(void);
ulong	swap1(ulong*);
void	touser(ulong);
void	trap(Ureg*);
void	trapinit(void);
#define	wbflush()	/* mips compatibility */
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
