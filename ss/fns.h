#include "../port/portfns.h"

void	cacheinit(void);
void	clearftt(ulong);
#define	clearmmucache()
void	clockinit(void);
void	disabfp(void);
void	enabfp(void);
void	evenaddr(ulong);
char*	excname(ulong);
void	faultasync(Ureg*);
void	faultsparc(Ureg*);
void	flushcpucache(void);
#define	flushpage(x)
int	fpcr(int);
int	fpquiet(void);
void	fpregrestore(char*);
void	fpregsave(char*);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
int	fptrap(void);
int	getfpq(ulong*);
ulong	getfsr(void);
ulong	(*getsysspace)(ulong);
void	intrinit(void);
void	ioinit(void);
int	kbdstate(IOQ*, int);
void	kbdclock(void);
void	kbdrepeat(int);
KMap*	kmap(Page*);
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)
void	kmapinit(void);
KMap*	kmappa(ulong, ulong);
KMap*	kmapperm(Page*);
int	kprint(char*, ...);
void	kproftimer(ulong);
void	kunmap(KMap*);
void	lanceintr(void);
void	lancesetup(Lance*);
void	lancetoggle(void);
void	mmuinit(void);
void	mousebuttons(int);
void	printinit(void);
#define	procrestore(p)
#define	procsave(p)
#define	procsetup(x)	((p)->fpstate = FPinit)
void	putcxsegm(int, ulong, int);
void	(*putenab)(ulong);
void	putpmeg(ulong, ulong);
void	putsegm(ulong, int);
void	(*putsysspace)(ulong, ulong);
void	putstr(char*);
void	putstrn(char*, long);
void	puttbr(ulong);
void	systemreset(void);
void	restfpregs(FPsave*, ulong);
void	screeninit(void);
void	screenputc(int);
void	screenputs(char*, int);
void	spldone(void);
ulong	tas(ulong*);
void	touser(ulong);
void	trap(Ureg*);
void	trapinit(void);
#define	wbflush()	/* mips compatibility */
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
#define getcallerpc(x)	(*(ulong*)(x))
