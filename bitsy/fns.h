#include "../port/portfns.h"

int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
#define	clearmmucache()				/* x86 doesn't have one */
void	clockinit(void);
#define	coherence()
void	delay(int);
void	evenaddr(ulong);
void	exceptionvectors(void);
ulong	getfar(void);
ulong	getfsr(void);
#define	getpgcolor(a)	0
void	idle(void);
#define	idlehands()			/* nothing to do in the runproc */
void	intrenable(int, void (*)(Ureg*, void*), void*, char*);
int	iprint(char*, ...);
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
void	reset(void);
void	screeninit(void);
int	screenprint(char*, ...);			/* debugging */
void	(*screenputs)(char*, int);
void	touser(void*);
void	trapinit(void);
int	tas(void*);
void	uartsetup(void);
void	wbflush(void);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a))

#define	dcflush(a, b)
