#include "../port/portfns.h"

void	aamloop(int);
void	addconf(char*, char*);
void	addscsilink(char*, Scsiio (*)(int, ISAConf*));
void	archinit(void);
void	bootargs(ulong);
int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
#define	clearmmucache()				/* x86 doesn't have one */
void	clockintr(Ureg*, void*);
void	(*coherence)(void);
void	cpuid(char*, int*, int*);
int	cpuidentify(void);
void	cpuidprint(void);
void	delay(int);
int	dmacount(int);
int	dmadone(int);
void	dmaend(int);
void	dmainit(int);
long	dmasetup(int, void*, long, int);
#define	evenaddr(x)				/* x86 doesn't care */
void	fpenv(FPsave*);
void	fpinit(void);
void	fpoff(void);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
ulong	fpstatus(void);
ulong	getcr0(void);
ulong	getcr2(void);
ulong	getcr3(void);
ulong	getcr4(void);
char*	getconf(char*);
int	i8042auxcmd(int);
void	i8042auxenable(void (*)(int, int));
void	i8042reset(void);
void	i8253init(int);
void	i8253enable(void);
void	i8259init(void);
int	i8259enable(int, int, Irqctl*);
void	idle(void);
int	inb(int);
void	insb(int, void*, int);
ushort	ins(int);
void	inss(int, void*, int);
ulong	inl(int);
void	insl(int, void*, int);
void	intrenable(int, void (*)(Ureg*, void*), void*, int);
int	iprint(char*, ...);
int	isaconfig(char*, int, ISAConf*);
void	kbdinit(void);
void	lgdt(ushort[3]);
void	lidt(ushort[3]);
void	links(void);
void	ltr(ulong);
void	mathinit(void);
#define mmuflushtlb(pdb) putcr3(pdb)
void	meminit(ulong);
void	mmuinit(void);
ulong	mmukmap(ulong, ulong, int);
int	mmukmapsync(ulong);
#define	mmunewpage(x)
ulong*	mmuwalk(ulong*, ulong, int);
void	ns16552install(void);
void	ns16552special(int, int, Queue**, Queue**, int (*)(Queue*, int));
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, ulong);
void	outsl(int, void*, int);
ulong	pcibarsize(Pcidev*, int);
int	pcicfgr8(Pcidev*, int);
int	pcicfgr16(Pcidev*, int);
int	pcicfgr32(Pcidev*, int);
void	pcicfgw8(Pcidev*, int, int);
void	pcicfgw16(Pcidev*, int, int);
void	pcicfgw32(Pcidev*, int, int);
void	pcihinv(Pcidev*);
Pcidev* pcimatch(Pcidev*, int, int);
void	pcireset(void);
PCMmap*	pcmmap(int, ulong, int, int);
int	pcmspecial(char*, ISAConf*);
void	pcmspecialclose(int);
void	pcmunmap(int, PCMmap*);
void	printcpufreq(void);
#define	procrestore(p)
void	procsave(Proc*);
void	procsetup(Proc*);
void	putcr3(ulong);
void	putcr4(ulong);
void	rdmsr(int, ulong*, ulong*);
void	screeninit(void);
int	screenprint(char*, ...);			/* debugging */
void	screenputs(char*, int);
void	touser(void*);
void	trapinit(void);
int	tas(void*);
ulong	umbmalloc(ulong, int, int);
void	umbfree(ulong, int);
ulong	umbrwmalloc(ulong, int, int);
void	umbrwfree(ulong, int);
ulong	upamalloc(ulong, int, int);
void	upafree(ulong, int);
void	vectortable(void);
void	wrmsr(int, ulong, ulong);
void	wbflush(void);
int	xchgw(ushort*, int);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define getcallerpc(x)	(((ulong*)(&x))[-1])
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)

#define	dcflush(a, b)
