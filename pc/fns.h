#include "../port/portfns.h"

#define	clearmmucache()		/* 386 doesn't have one */
void	clock(Ureg*);
void	clockinit(void);
void	delay(int);
#define	evenaddr(x)		/* 386 doesn't care */
void	fault386(Ureg*);
void	floppyinit(void);
void	floppyintr(Ureg*);
long	floppyseek(int, long);
long	floppyread(int, void*, long);
#define	flushvirt();
void	idle(void);
int	inb(int);
void	intr0(void);
void	intr1(void);
void	intr2(void);
void	intr3(void);
void	intr4(void);
void	intr5(void);
void	intr6(void);
void	intr7(void);
void	intr8(void);
void	intr9(void);
void	intr10(void);
void	intr11(void);
void	intr12(void);
void	intr13(void);
void	intr14(void);
void	intr15(void);
void	intr16(void);
void	intr17(void);
void	intr18(void);
void	intr19(void);
void	intr20(void);
void	intr21(void);
void	intr22(void);
void	intr23(void);
void	intr64(void);
void	intrbad(void);
void	kbdinit(void);
void	kbdintr(Ureg*);
void	lgdt(Segdesc*, int);
void	lidt(Segdesc*, int);
void	mmuinit(void);
void	outb(int, int);
void	prhex(ulong);
#define	procrestore(x,y)
#define	procsave(x,y)
#define	procsetup(p)	((p)->fpstate = FPinit)
void	screeninit(void);
void	screenputc(int);
void	screenputs(char*, int);
void	setvec(int, void (*)(Ureg*), int);
void	systrap(void);
void	touser(void);
void	trapinit(void);
int	tas(Lock*);
#define	waserror()	(u->nerrlab++, setlabel(&u->errlab[u->nerrlab-1]))
