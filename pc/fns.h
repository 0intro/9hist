#include "../port/portfns.h"

void	addconf(char*, char*);
#define	affinity(x) m
void	bbinit(void);
void	bigcursor(void);
void	bootargs(ulong);
#define	clearmmucache()		/* 386 doesn't have one */
void	clockinit(void);
void	config(int);
int	cpuspeed(int);
void	delay(int);
void	dmaend(int);
void	dmainit(void);
long	dmasetup(int, void*, long, int);
#define	evenaddr(x)		/* 386 doesn't care */
void	faultinit(void);
void	fclock(Ureg*);
void	fclockinit(void);
void	fpenv(FPsave*);
void	fpinit(void);
void	fpoff(void);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
ulong	fpstatus(void);
ulong	getcr0(void);
ulong	getcr2(void);
char*	getconf(char*);
ulong	getstatus(void);
void	hardclock(void);
void	i8042a20(void);
void	i8042reset(void);
void	ident(void);
void	idle(void);
int	inb(int);
void	insb(int, void*, int);
ushort	ins(int);
void	inss(int, void*, int);
ulong	inl(int);
void	insl(int, void*, int);
int	isaconfig(char*, int, ISAConf*);
ulong	isamem(int);
void	kbdinit(void);
void*	l0update(uchar*, uchar*, long);
void*	l1update(uchar*, uchar*, long);
void*	l2update(uchar*, uchar*, long);
long*	mapaddr(ulong);
void	mathinit(void);
void	meminit(void);
void	mmuinit(void);
#define	mmunewpage(x)
int	modem(int);
void	mousectl(char*);
uchar	nvramread(int);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, ulong);
void	outsl(int, void*, int);
void	pcicreset(void);
int	pcmio(int, ISAConf*);
long	pcmread(int, int, void*, long, ulong);
int	pcmspecial(int);
void	pcmspecialclose(int);
long	pcmwrite(int, int, void*, long, ulong);
void	prflush(void);
void	prhex(ulong);
void	procrestore(Proc*);
void	procsave(Proc*);
void	procsetup(Proc*);
void	ps2poll(void);
void	putgdt(Segdesc*, int);
void	putidt(Segdesc*, int);
void	putcr3(ulong);
void	puttr(ulong);
void	screeninit(void);
void	screenputs(char*, int);
int	serial(int);
void	setvec(int, void (*)(Ureg*, void*), void*);
void	syscall(Ureg*, void*);
void	systrap(void);
void	toscreen(void*);
void	touser(void*);
void	trapinit(void);
int	tas(void*);
void	uartclock(void);
void	vgainit(void);
#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define	kmapperm(x)	kmap(x)
#define getcallerpc(x)	(*(ulong*)(x))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)

void	NS16552special(int, int, Queue**, Queue**, int (*)(Queue*, int));
int	NS16552m3mouse(Queue*, int);
int	NS16552mouse(Queue*, int);
