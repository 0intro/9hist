#include "../port/portfns.h"

void	meminit(void);
#define	clearmmucache()		/* 386 doesn't have one */
void	clock(Ureg*);
void	clockinit(void);
void	config(int);
void	delay(int);
void	dmaend(int);
long	dmasetup(int, void*, long, int);
#define	evenaddr(x)		/* 386 doesn't care */
void	fault386(Ureg*);
void	faultinit(void);
void	fclock(Ureg*);
void	fclockinit(void);
void	fpinit(void);
void	fpoff(void);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
ulong	fpstatus(void);
ulong	getcr0(void);
ulong	getcr2(void);
void	idle(void);
int	inb(int);
void	inss(int, void*, int);
void	kbdinit(void);
void	mathinit(void);
void	mmuinit(void);
int	modem(int);
void	outb(int, int);
void	outss(int, void*, int);
void	prhex(ulong);
void	procrestore(Proc*, uchar*);
void	procsave(uchar*, int);
void	procsetup(Proc*);
void	putgdt(Segdesc*, int);
void	putidt(Segdesc*, int);
void	putcr3(ulong);
void	puttr(ulong);
void	screeninit(void);
void	screenputc(int);
void	screenputs(char*, int);
int	serial(int);
void	setvec(int, void (*)(Ureg*));
void	systrap(void);
void	touser(void);
void	trapinit(void);
int	tas(Lock*);
void	uartintr0(Ureg*);
void	vgainit(void);
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
