#include "../port/portfns.h"

int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
void	cleancache(void);
void	cleanaddr(ulong);
#define	clearmmucache()				/* x86 doesn't have one */
void	clockinit(void);
#define	coherence()
void	delay(int);
void	evenaddr(ulong);
void	flushcache(void);
void	flushmmu(void);
ulong	getfar(void);
ulong	getfsr(void);
#define	getpgcolor(a)	0
void	h3650uartsetup(void);
void	idle(void);
#define	idlehands()			/* nothing to do in the runproc */
void	intrenable(int, void (*)(Ureg*, void*), void*, char*);
int	iprint(char*, ...);
void	irpower(int);
void	lcdpower(int);
void	mappedIvecEnable(void);
void	mappedIvecDisable(void);
void*	mapspecial(ulong, int);
void	meminit(void);
void	mmuinit(void);
void	mmuenable(void);
void	mmudisable(void);
void	noted(Ureg*, ulong);
int	notify(Ureg*);
#define	procrestore(p)
void	procsave(Proc*);
void	procsetup(Proc*);
void	putdac(ulong);
void	putttb(ulong);
void	putpid(ulong);
void	qpanic(char *, ...);
void	reset(void);
void	rs232power(int);
Uart*	uartsetup(PhysUart*, void*, ulong, char*);
void	sa1100_uartsetup(void);
void	screeninit(void);
int	screenprint(char*, ...);			/* debugging */
void	(*screenputs)(char*, int);
void	setr13(int, ulong*);
void	touser(void*);
void	trapdump(char *tag);
void	trapinit(void);
int	tas(void*);
int	uartstageoutput(Uart*);
void	vectors(void);
void	vtable(void);
void	wbflush(void);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a))

#define	dcflush(a, b)
