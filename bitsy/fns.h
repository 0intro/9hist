#include "../port/portfns.h"

int	cistrcmp(char*, char*);
int	cistrncmp(char*, char*, int);
#define	clearmmucache()				/* x86 doesn't have one */
void	clockintr(Ureg*, void*);
void	clockintrsched(void);
void	(*coherence)(void);
void	delay(int);
void	evenaddr(ulong);
ulong	getfar(void);
ulong	getfsr(void);
#define	getpgcolor(a)	0
void	idle(void);
#define	idlehands()			/* nothing to do in the runproc */
int	iprint(char*, ...);
void	meminit(void);
void	mmuinit(void);
#define	procrestore(p)
void	procsave(Proc*);
void	procsetup(Proc*);
void	putdac(ulong);
void	putttb(ulong);
void	putpid(ulong);
void	screeninit(void);
int	screenprint(char*, ...);			/* debugging */
void	(*screenputs)(char*, int);
void	touser(void*);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
int	tas(void*);
void	wbflush(void);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define KADDR(a)	((void*)((ulong)(a)|KZERO))
#define PADDR(a)	((ulong)(a))

#define	dcflush(a, b)
