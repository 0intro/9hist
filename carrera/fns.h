#include "../port/portfns.h"

void		DEBUG(void);

void		addportintr(int(*)(void));
#define		affinity(x) m
void		allflush(void*, ulong);
void		arginit(void);
int		busprobe(ulong);
void		cleancache(void);
void		clearmmucache(void);
void		clock(Ureg*);
void		clockinit(void);
ulong		confeval(char*);
void		confprint(void);
void		confread(void);
void		confset(char*);
int		conschar(void);
void		consoff(void);
int		consputc(int);
void		dcflush(void*, ulong);
void		dcinvalidate(void*, ulong);
void		epcenable(ulong);
void		epcinit(int, int);
void		evenaddr(ulong);
void		faultmips(Ureg*, int, int);
ulong		fcr31(void);
void		firmware(int);
#define		flushpage(s)		icflush((void*)(s), BY2PG)
void		fptrap(Ureg*);
ulong		getcallerpc(void*);
int		getline(char*, int);
void		getnveaddr(void*);
int		getnvram(ulong, void *, int);
ulong		getstatus(void);
void		gettlb(int, ulong*);
int		gettlbp(ulong, ulong*);
ulong		gettlbvirt(int);
void		gotopc(ulong);
void		hinv(void);
void		icdirty(void *, ulong);
void		icflush(void *, ulong);
void		intr(Ureg*);
void		ioinit(int);
int		iprint(char*, ...);
int		kbdinit(void);
void		kbdintr(void);
void		kfault(Ureg*);
KMap*		kmap(Page*);
void		kmapinit(void);
void		kmapinval(void);
int		kprint(char*, ...);
void		kproftimer(ulong);
void		kunmap(KMap*);
void		launchinit(void);
void		launch(int);
void		lightbits(int, int);
ulong		machstatus(void);
void		mmunewpage(Page*);
void		mouseintr(void);
void		mntdump(void);
void		newstart(void);
int		newtlbpid(Proc*);
void		nonettoggle(void);
void		novme(int);
void		online(void);
Block*		prepend(Block*, int);
void		prflush(void);
ulong		prid(void);
void		printinit(void);
#define		procrestore(p)
void		procsave(Proc*);
#define		procsetup(p)		((p)->fpstate = FPinit)
void		purgetlb(int);
int		putnvram(ulong, void*, int);
Softtlb*	putstlb(ulong, ulong);
int		puttlb(ulong, ulong, ulong);
void		puttlbx(int, ulong, ulong, ulong, int);
void*		pxalloc(ulong);
void*		pxspanalloc(ulong, int, ulong);
void		rdbginit(void);
ulong		rdcount(void);
int		readlog(ulong, char*, ulong);
void		restfpregs(FPsave*, ulong);
void		screeninit(void);
void		screenputs(char*, int);
long		syscall(Ureg*);
void		syslog(char*, int);
void		sysloginit(void);
int		tas(ulong*);
void		tlbinit(void);
ulong		tlbvirt(void);
void		touser(void*);
ulong		uvmove(uvlong*, uvlong*);
void		vecinit(void);
void		vector0(void);
void		vector100(void);
void		vector180(void);
void		vmereset(void);
void		uvst(void*, void*);
void		uvld(void*, void*);
void		wbflush(void);
void		wrcompare(ulong);
void		Xdelay(int);

void		serialinit(void);
void		NS16552special(int, int, Queue**, Queue**, int (*)(Queue*, int));
void		NS16552setup(ulong, ulong);
void		NS16552intr(int);
void		etherintr(void);
void		iomapinit(void);
void		enetaddr(uchar*);

#define	waserror()		setlabel(&up->errlab[up->nerrlab++])
#define	kmapperm(x)		kmap(x)

#define KADDR(a)	((void*)((ulong)(a)|KSEG0))
#define KADDR1(a)	((void*)((ulong)(a)|KSEG1))
#define PADDR(a)	((ulong)(a)&~KSEGM)

#define hnputl(p, v)	((*(ulong*)p) = v)
#define hnputs(p, v)	((*(ushort*)p) = v)
#define nhgetl(p)	((*(ulong*)p))
#define nhgets(p)	((*(ushrt*)p))

void	ifwrite(void*, Block*);
void*	ifinit(int);
void	fiberint(int);
