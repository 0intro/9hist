#include "../port/portfns.h"

void	aamloop(int);
void	addconf(char*, char*);
void	addscsilink(char*, Scsiio (*)(int, ISAConf*));
void	bbinit(void);
void	bigcursor(void);
void	bootargs(ulong);
int	cistrcmp(char*, char*);
#define	clearmmucache()		/* 386 doesn't have one */
void	clockinit(void);
void	config(int);
int	cpuspeed(int);
void	delay(int);
int	dmadone(int);
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
ulong	getcr3(void);
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
int	iprint(char*, ...);
int	isaconfig(char*, int, ISAConf*);
ulong	getisa(ulong, int, int);
void	putisa(ulong, int);
ulong	getspace(int, int);
void	kbdinit(void);
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
PCMmap*	pcmmap(int, ulong, int, int);
int	pcmspecial(char*, ISAConf*);
void	pcmspecialclose(int);
void	pcmunmap(int, PCMmap*);
void	prflush(void);
void	prhex(ulong);
void	printcpufreq(void);
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
void	uartpoll(void);
void	vgainit(void);
void	vgasavecrash(uchar*, int);
void	vgarestorecrash(uchar*, int);
int	x86(void);
int	x86cpuid(int*, int*);
int	xchgw(ushort*, int);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define	kmapperm(x)	kmap(x)
#define getcallerpc(x)	(((ulong*)(&x))[-1])
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a)&~KZERO)

void	ns16552install(void);
void	ns16552special(int, int, Queue**, Queue**, int (*)(Queue*, int));

void	hnputl(void*, ulong v);
void	hnputs(void*, ushort v);
ulong	nhgetl(void*);
ushort	nhgets(void*);

void	ifwrite(void*, Block*, int);
void*	ifinit(int);
ulong	ifaddr(void*);
void	filiput(Block*);
void	fiberint(Ureg*, void*);
ulong	fwblock(ulong, void*, ulong);
ulong	frblock(ulong, void*, ulong);
void	freset(void*);
void	ifree(void*);
void	ifflush(void*);
Block*	iallocb(int);
void*	ifroute(ulong);
ulong	ifunroute(ulong);
void	parseip(char*, char*);

#define	dcflush(a, b)

int scsibtissue(int, int, uchar*, int, uchar*, int);
Scsiio scsibt24(int, int, ISAConf*);
Scsiio scsibt32(int, int, ISAConf*);
