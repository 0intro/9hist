#include "../port/portfns.h"

void	archinit(void);
int	brgalloc(void);
void	brgfree(int);
ulong	baudgen(int, int);
int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
void	clockcheck(void);
void	clockinit(void);
void	clockintr(Ureg*);
void	clrfptrap(void);
#define coherence()
void	cpminit(void);
void	cpuidentify(void);
void	cpuidprint(void);
void	dcflush(void*, ulong);
void	delay(int);
ulong	draminit(ulong*);
void	dtlbmiss(void);
void	dtlberror(void);
void	dumpregs(Ureg*);
void	delayloopinit(void);
void	eieio(void);
void	evenaddr(ulong);
void	faultpower(Ureg*, ulong addr, int read);
void	firmware(void);
void	fpinit(void);
int	fpipower(Ureg*);
void	fpoff(void);
ulong	fpstatus(void);
char*	getconf(char*);
ulong	getdar(void);
ulong	getdec(void);
ulong	getdepn(void);
ulong	getdsisr(void);
ulong	getimmr(void);
ulong	getmsr(void);
ulong	getpvr(void);
ulong	gettbl(void);
ulong	gettbu(void);
void	gotopc(ulong);
void	i8250console(void);
void	icflush(void*, ulong);
void	idle(void);
#define	idlehands()			/* nothing to do in the runproc */
int	inb(int);
void	insb(int, void*, int);
ushort	ins(int);
void	inss(int, void*, int);
ulong	inl(int);
void	insl(int, void*, int);
void	intr(Ureg*);
void	intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
int	intrstats(char*, int);
void	intrvec(void);
int	iprint(char*, ...);
void	itlbmiss(void);
int	isaconfig(char*, int, ISAConf*);
void	kbdinit(void);
void	kbdreset(void);
void	kernelmmu(void);
#define	kmapinval()
void	links(void);
void	mathinit(void);
void	mmuinit(void);
ulong*	mmuwalk(ulong*, ulong, int);
void	mpicenable(int);
void	mpiceoi(void);
int	mpicintack(void);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, ulong);
void	outsl(int, void*, int);
int	pciscan(int, Pcidev **);
ulong pcibarsize(Pcidev *, int);
int	pcicfgr8(Pcidev*, int);
int	pcicfgr16(Pcidev*, int);
int	pcicfgr32(Pcidev*, int);
void	pcicfgw8(Pcidev*, int, int);
void	pcicfgw16(Pcidev*, int, int);
void	pcicfgw32(Pcidev*, int, int);
void pciclrbme(Pcidev*);
void	pcihinv(Pcidev*);
uchar pciipin(Pcidev *, uchar);
Pcidev* pcimatch(Pcidev*, int, int);
Pcidev* pcimatchtbdf(int);
void	pcireset(void);
void	pcisetbme(Pcidev*);
int		pcmspecial(char*, ISAConf*);
void	pcmspecialclose(int);
#define	procrestore(p)
void	powerdownled(void);
void	powerupled(void);
void	procsave(Proc*);
void	procsetup(Proc*);
void	putdec(ulong);
void	putmsr(ulong);
void	putcasid(ulong);
void	raveninit(void);
void	screeninit(void);
void	setpanic(void);
int	screenprint(char*, ...);			/* debugging */
ulong	sdraminit(ulong*);
int	segflush(void*, ulong);
void	spireset(void);
long	spioutin(void*, long, void*);
int	tas(void*);
uchar*	tarlookup(uchar *addr, char *file, int *dlen);
void	touser(void*);
void	trapinit(void);
void	trapvec(void);
void	tlbflush(ulong);
void	tlbflushall(void);
void	uartinstall(void);
void	uartwait(void);	/* debugging */
int unsac(uchar *dst, uchar *src, int n, int nsrc);
#define	userureg(ur) (((ur)->status & MSR_PR) != 0)
void	wbflush(void);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)
