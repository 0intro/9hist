#include "../port/portfns.h"

void	a20enable(void);
#define	clearmmucache()		/* 386 doesn't have one */
void	clock(Ureg*);
void	clockinit(void);
void	delay(int);
void	dmaend(int);
long	dmasetup(int, void*, long, int);
#define	evenaddr(x)		/* 386 doesn't care */
void	fault386(Ureg*);
void	faultinit(void);
#define	flushvirt();
void	fpsave(FPsave*);
void	fprestore(FPsave*);
ulong	getcr2(void);
void	idle(void);
int	inb(int);
void	kbdinit(void);
void	kbdintr(Ureg*);
void	mmuinit(void);
void	outb(int, int);
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
void	setvec(int, void (*)(Ureg*));
void	systrap(void);
void	touser(void);
void	trapinit(void);
int	tas(Lock*);
void	vgainit(void);
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
